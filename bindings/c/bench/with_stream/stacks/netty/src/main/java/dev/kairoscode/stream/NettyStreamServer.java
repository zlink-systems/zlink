package dev.kairoscode.stream;

import io.netty.bootstrap.ServerBootstrap;
import io.netty.buffer.ByteBuf;
import io.netty.channel.Channel;
import io.netty.channel.ChannelFutureListener;
import io.netty.channel.ChannelHandlerContext;
import io.netty.channel.ChannelInboundHandlerAdapter;
import io.netty.channel.ChannelInitializer;
import io.netty.channel.ChannelOption;
import io.netty.channel.EventLoopGroup;
import io.netty.channel.nio.NioEventLoopGroup;
import io.netty.channel.socket.SocketChannel;
import io.netty.channel.socket.nio.NioServerSocketChannel;
import io.netty.handler.codec.LengthFieldBasedFrameDecoder;
import io.netty.handler.codec.TooLongFrameException;
import io.netty.util.ReferenceCountUtil;

import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;

public final class NettyStreamServer
{
    private static final int MIN_PAYLOAD_SIZE = 16;
    private static final int MAX_PAYLOAD_SIZE = 4 * 1024 * 1024;
    private static final byte[] MSG_NAME = "stream.echo".getBytes();
    private static final int PREFIX_SIZE = 6;

    private NettyStreamServer()
    {
    }

    private static final class ServerOptions
    {
        String host = "0.0.0.0";
        int port = 38006;
        int size = 1024;
        int sndbuf = 1024 * 1024;
        int rcvbuf = 1024 * 1024;
        int backlog = 32768;
        int tcpNoDelay = 1;
        int ioThreads = 4;

        static ServerOptions parse(String[] args)
        {
            ServerOptions opt = new ServerOptions();
            for (int i = 0; i + 1 < args.length; i++) {
                String key = args[i];
                if (!key.startsWith("--"))
                    continue;
                String value = args[++i];
                switch (key) {
                    case "--host":
                        opt.host = value;
                        break;
                    case "--port":
                        opt.port = parseInt(value, opt.port, 1);
                        break;
                    case "--size":
                        opt.size = parseInt(value, opt.size, MIN_PAYLOAD_SIZE);
                        break;
                    case "--sndbuf":
                        opt.sndbuf = parseInt(value, opt.sndbuf, 1);
                        break;
                    case "--rcvbuf":
                        opt.rcvbuf = parseInt(value, opt.rcvbuf, 1);
                        break;
                    case "--backlog":
                        opt.backlog = parseInt(value, opt.backlog, 1);
                        break;
                    case "--tcp-nodelay":
                        opt.tcpNoDelay = parseInt(value, opt.tcpNoDelay, 0);
                        break;
                    case "--io-threads":
                        opt.ioThreads = parseInt(value, opt.ioThreads, 1);
                        break;
                    default:
                        break;
                }
            }
            return opt;
        }

        private static int parseInt(String text, int fallback, int min)
        {
            int parsed;
            try {
                parsed = Integer.parseInt(text);
            } catch (Exception ignore) {
                return fallback;
            }
            if (parsed < min)
                return min;
            return parsed;
        }
    }

    private static final class Metrics
    {
        final AtomicLong recvMsgs = new AtomicLong();
        final AtomicLong parseError = new AtomicLong();
        final AtomicLong protocolError = new AtomicLong();
        final AtomicLong sendError = new AtomicLong();
        final AtomicLong activeConnections = new AtomicLong();

        void onOpen()
        {
            activeConnections.incrementAndGet();
        }

        void onClose()
        {
            long prev;
            do {
                prev = activeConnections.get();
                if (prev <= 0)
                    return;
            } while (!activeConnections.compareAndSet(prev, prev - 1));
        }
    }

    private static final class EchoHandler extends ChannelInboundHandlerAdapter
    {
        private final ServerOptions opt;
        private final Metrics metrics;

        EchoHandler(ServerOptions opt_, Metrics metrics_)
        {
            opt = opt_;
            metrics = metrics_;
        }

        @Override
        public void channelActive(ChannelHandlerContext ctx)
        {
            metrics.onOpen();
        }

        @Override
        public void channelInactive(ChannelHandlerContext ctx)
        {
            metrics.onClose();
        }

        @Override
        public void channelRead(ChannelHandlerContext ctx, Object msg)
        {
            if (!(msg instanceof ByteBuf)) {
                metrics.parseError.incrementAndGet();
                ReferenceCountUtil.release(msg);
                return;
            }
            ByteBuf frame = (ByteBuf) msg;
            final int headerSize = frame.getUnsignedShort(0);
            final int bodySize = frame.readableBytes() - PREFIX_SIZE - headerSize;
            boolean headerMatch = headerSize == MSG_NAME.length;
            for (int i = 0; headerMatch && i < MSG_NAME.length; i++) {
                headerMatch = frame.getByte(PREFIX_SIZE + i) == MSG_NAME[i];
            }
            if (!headerMatch) {
                metrics.parseError.incrementAndGet();
                metrics.protocolError.incrementAndGet();
                frame.release();
                ctx.close();
                return;
            }
            if (bodySize < MIN_PAYLOAD_SIZE || bodySize > MAX_PAYLOAD_SIZE) {
                metrics.parseError.incrementAndGet();
                metrics.protocolError.incrementAndGet();
                frame.release();
                ctx.close();
                return;
            }
            if (bodySize > opt.size) {
                metrics.protocolError.incrementAndGet();
                frame.release();
                ctx.close();
                return;
            }

            metrics.recvMsgs.incrementAndGet();
            ByteBuf reply = ctx.alloc().buffer(frame.readableBytes());
            reply.writeBytes(frame, frame.readerIndex(), frame.readableBytes());
            frame.release();
            ctx.write(reply).addListener((ChannelFutureListener) future -> {
                if (!future.isSuccess()) {
                    metrics.sendError.incrementAndGet();
                    future.channel().close();
                }
            });
        }

        @Override
        public void channelReadComplete(ChannelHandlerContext ctx)
        {
            ctx.flush();
        }

        @Override
        public void exceptionCaught(ChannelHandlerContext ctx, Throwable cause)
        {
            if (cause instanceof TooLongFrameException) {
                metrics.parseError.incrementAndGet();
                metrics.protocolError.incrementAndGet();
            } else {
                metrics.sendError.incrementAndGet();
            }
            ctx.close();
        }
    }

    private static void printMetricLine(ServerOptions opt, Metrics metrics)
    {
        System.out.printf(
          "METRIC stack=netty mode=echo size=%d recv_msgs=%d parse_error=%d protocol_error=%d send_error=%d connections=%d%n",
          opt.size,
          metrics.recvMsgs.get(),
          metrics.parseError.get(),
          metrics.protocolError.get(),
          metrics.sendError.get(),
          metrics.activeConnections.get());
    }

    public static void main(String[] args) throws Exception
    {
        if (args.length == 0) {
            System.out.println("test_scenario_stream_netty: no args -> skip");
            return;
        }

        final ServerOptions opt = ServerOptions.parse(args);
        if (opt.size > MAX_PAYLOAD_SIZE) {
            System.err.printf("netty stream: size too large %d%n", opt.size);
            return;
        }

        final Metrics metrics = new Metrics();
        final EventLoopGroup boss = new NioEventLoopGroup(1);
        final EventLoopGroup workers = new NioEventLoopGroup(Math.max(1, opt.ioThreads));
        final AtomicBoolean printed = new AtomicBoolean(false);
        final AtomicBoolean shutdown = new AtomicBoolean(false);
        final Channel[] holder = new Channel[1];

        Runtime.getRuntime().addShutdownHook(new Thread(() -> {
            if (shutdown.compareAndSet(false, true)) {
                try {
                    Channel ch = holder[0];
                    if (ch != null)
                        ch.close().syncUninterruptibly();
                } catch (Exception ignore) {
                }
                workers.shutdownGracefully().syncUninterruptibly();
                boss.shutdownGracefully().syncUninterruptibly();
                if (printed.compareAndSet(false, true))
                    printMetricLine(opt, metrics);
            }
        }));

        try {
            ServerBootstrap bootstrap = new ServerBootstrap();
            bootstrap.group(boss, workers)
              .channel(NioServerSocketChannel.class)
              .option(ChannelOption.SO_REUSEADDR, true)
              .option(ChannelOption.SO_BACKLOG, Math.max(1, opt.backlog))
              .childOption(ChannelOption.TCP_NODELAY, opt.tcpNoDelay != 0)
              .childOption(ChannelOption.SO_SNDBUF, Math.max(1, opt.sndbuf))
              .childOption(ChannelOption.SO_RCVBUF, Math.max(1, opt.rcvbuf))
              .childHandler(new ChannelInitializer<SocketChannel>()
              {
                  @Override
                  protected void initChannel(SocketChannel ch)
                  {
                      ch.pipeline().addLast(
                        new LengthFieldBasedFrameDecoder(
                          MAX_PAYLOAD_SIZE + PREFIX_SIZE + MSG_NAME.length,
                          2, 4, MSG_NAME.length, 0));
                      ch.pipeline().addLast(new EchoHandler(opt, metrics));
                  }
              });

            Channel ch = bootstrap.bind(opt.host, opt.port).sync().channel();
            holder[0] = ch;
            ch.closeFuture().sync();
        } finally {
            if (shutdown.compareAndSet(false, true)) {
                workers.shutdownGracefully().syncUninterruptibly();
                boss.shutdownGracefully().syncUninterruptibly();
                if (printed.compareAndSet(false, true))
                    printMetricLine(opt, metrics);
            }
        }
    }
}
