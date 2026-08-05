package perfcommon

import (
	"crypto/rand"
	"crypto/rsa"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/binary"
	"encoding/pem"
	"fmt"
	"io"
	"math/big"
	"net"
	"net/url"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"time"

	"github.com/gorilla/websocket"
	zlink "zlink.systems/zlink/v11"
)

type StreamConn interface {
	io.ReadWriteCloser
	SetDeadline(time.Time) error
}

type PacketConn interface {
	io.ReadWriteCloser
	SetDeadline(time.Time) error
}

type tlsAssetSet struct {
	caPath   string
	certPath string
	keyPath  string
	caPEM    string
	certPEM  string
	keyPEM   string
	rootCAs  *x509.CertPool
}

var (
	tlsAssetsOnce sync.Once
	tlsAssets     tlsAssetSet
	tlsAssetsErr  error
)

func EnsureTLSAssets() (tlsAssetSet, error) {
	tlsAssetsOnce.Do(func() {
		tlsAssets, tlsAssetsErr = buildTLSAssets()
	})
	return tlsAssets, tlsAssetsErr
}

func ConfigureTLSServer(socket interface {
	SetTLSServer(string, string, bool) error
}, transport string) error {
	if transport != "tls" && transport != "wss" {
		return nil
	}
	assets, err := EnsureTLSAssets()
	if err != nil {
		return err
	}
	return socket.SetTLSServer(assets.certPath, assets.keyPath, false)
}

func ConfigureTLSClient(socket interface {
	SetTLSClient(string, string, bool) error
}, transport string) error {
	if transport != "tls" && transport != "wss" {
		return nil
	}
	assets, err := EnsureTLSAssets()
	if err != nil {
		return err
	}
	return socket.SetTLSClient(assets.caPath, "localhost", false)
}

func buildTLSAssets() (tlsAssetSet, error) {
	if assets, ok, err := resolveRepoTLSAssets(); err != nil {
		return tlsAssetSet{}, err
	} else if ok {
		return assets, nil
	}

	dir, err := os.MkdirTemp("", "zlink-perf-tls-*")
	if err != nil {
		return tlsAssetSet{}, err
	}

	caKey, err := rsa.GenerateKey(rand.Reader, 2048)
	if err != nil {
		return tlsAssetSet{}, err
	}
	caTpl := &x509.Certificate{
		SerialNumber: big.NewInt(1),
		Subject: pkix.Name{
			CommonName:   "ZLink Perf CA",
			Organization: []string{"ZLink"},
			Country:      []string{"US"},
		},
		NotBefore:             time.Now().Add(-time.Hour),
		NotAfter:              time.Now().Add(10 * 365 * 24 * time.Hour),
		KeyUsage:              x509.KeyUsageCertSign | x509.KeyUsageCRLSign,
		IsCA:                  true,
		BasicConstraintsValid: true,
	}
	caDER, err := x509.CreateCertificate(rand.Reader, caTpl, caTpl, &caKey.PublicKey, caKey)
	if err != nil {
		return tlsAssetSet{}, err
	}

	serverKey, err := rsa.GenerateKey(rand.Reader, 2048)
	if err != nil {
		return tlsAssetSet{}, err
	}
	serverTpl := &x509.Certificate{
		SerialNumber: big.NewInt(2),
		Subject: pkix.Name{
			CommonName:   "localhost",
			Organization: []string{"ZLink"},
			Country:      []string{"US"},
		},
		NotBefore:   time.Now().Add(-time.Hour),
		NotAfter:    time.Now().Add(10 * 365 * 24 * time.Hour),
		KeyUsage:    x509.KeyUsageDigitalSignature | x509.KeyUsageKeyEncipherment,
		ExtKeyUsage: []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth},
		DNSNames:    []string{"localhost"},
		IPAddresses: []net.IP{net.ParseIP("127.0.0.1")},
	}
	serverDER, err := x509.CreateCertificate(rand.Reader, serverTpl, caTpl, &serverKey.PublicKey, caKey)
	if err != nil {
		return tlsAssetSet{}, err
	}

	caPath := filepath.Join(dir, "ca.pem")
	certPath := filepath.Join(dir, "server_cert.pem")
	keyPath := filepath.Join(dir, "server_key.pem")

	caPEM := pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: caDER})
	certPEM := pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: serverDER})
	keyPEM := pem.EncodeToMemory(&pem.Block{Type: "RSA PRIVATE KEY", Bytes: x509.MarshalPKCS1PrivateKey(serverKey)})

	if err := os.WriteFile(caPath, caPEM, 0o600); err != nil {
		return tlsAssetSet{}, err
	}
	if err := os.WriteFile(certPath, certPEM, 0o600); err != nil {
		return tlsAssetSet{}, err
	}
	if err := os.WriteFile(keyPath, keyPEM, 0o600); err != nil {
		return tlsAssetSet{}, err
	}

	pool := x509.NewCertPool()
	if !pool.AppendCertsFromPEM(caPEM) {
		return tlsAssetSet{}, fmt.Errorf("failed to append generated CA certificate")
	}

	return tlsAssetSet{
		caPath:   caPath,
		certPath: certPath,
		keyPath:  keyPath,
		caPEM:    string(caPEM),
		certPEM:  string(certPEM),
		keyPEM:   string(keyPEM),
		rootCAs:  pool,
	}, nil
}

func resolveRepoTLSAssets() (tlsAssetSet, bool, error) {
	roots := []string{}
	if cwd, err := os.Getwd(); err == nil {
		roots = append(roots, cwd)
	}
	if exe, err := os.Executable(); err == nil {
		roots = append(roots, filepath.Dir(exe))
	}
	for _, root := range roots {
		current := root
		for {
			candidate := filepath.Join(current, "core", "tests", "certs", "gen")
			caPath := filepath.Join(candidate, "ca.crt")
			certPath := filepath.Join(candidate, "server.crt")
			keyPath := filepath.Join(candidate, "server.key")
			if fileExists(caPath) && fileExists(certPath) && fileExists(keyPath) {
				caPEM, err := os.ReadFile(caPath)
				if err != nil {
					return tlsAssetSet{}, false, err
				}
				certPEM, err := os.ReadFile(certPath)
				if err != nil {
					return tlsAssetSet{}, false, err
				}
				keyPEM, err := os.ReadFile(keyPath)
				if err != nil {
					return tlsAssetSet{}, false, err
				}
				pool := x509.NewCertPool()
				if !pool.AppendCertsFromPEM(caPEM) {
					return tlsAssetSet{}, false, fmt.Errorf("failed to append repo CA certificate")
				}
				return tlsAssetSet{
					caPath:   caPath,
					certPath: certPath,
					keyPath:  keyPath,
					caPEM:    string(caPEM),
					certPEM:  string(certPEM),
					keyPEM:   string(keyPEM),
					rootCAs:  pool,
				}, true, nil
			}
			parent := filepath.Dir(current)
			if parent == current {
				break
			}
			current = parent
		}
	}
	return tlsAssetSet{}, false, nil
}

func fileExists(path string) bool {
	info, err := os.Stat(path)
	return err == nil && !info.IsDir()
}

func DialEndpoint(endpoint string) StreamConn {
	conn, err := openStreamEndpoint(endpoint)
	Must(err)
	return conn
}

func DialPacketEndpoint(endpoint string) PacketConn {
	conn, err := openPacketEndpoint(endpoint)
	Must(err)
	return conn
}

func FrameStreamPacketMessage(header, body *zlink.Message) *zlink.Message {
	if header == nil || body == nil {
		Must(fmt.Errorf("frame stream packet requires non-nil messages"))
	}
	headerData := header.Data()
	bodyData := body.Data()
	frame := make([]byte, 6+len(headerData)+len(bodyData))
	binary.BigEndian.PutUint16(frame[:2], uint16(len(headerData)))
	binary.BigEndian.PutUint32(frame[2:6], uint32(len(bodyData)))
	copy(frame[6:], headerData)
	copy(frame[6+len(headerData):], bodyData)
	msg, err := zlink.NewMessage(frame)
	Must(err)
	return msg
}

func openStreamEndpoint(endpoint string) (StreamConn, error) {
	u, err := url.Parse(endpoint)
	if err != nil || u.Scheme == "" {
		return openTCPStream(endpoint)
	}
	switch strings.ToLower(u.Scheme) {
	case "tcp":
		return openTCPStream(endpoint)
	case "tls":
		return openTLSStream(u.Host)
	case "ws":
		return openWSStream(endpoint, false)
	case "wss":
		return openWSStream(endpoint, true)
	default:
		return openTCPStream(endpoint)
	}
}

func openTCPStream(endpoint string) (StreamConn, error) {
	addr := endpoint
	if idx := strings.Index(addr, "://"); idx >= 0 {
		addr = addr[idx+3:]
	}
	if idx := strings.IndexByte(addr, '?'); idx >= 0 {
		addr = addr[:idx]
	}
	conn, err := net.DialTimeout("tcp", addr, 5*time.Second)
	if err != nil {
		return nil, err
	}
	return &framedNetConn{conn: conn}, nil
}

func openTLSStream(addr string) (StreamConn, error) {
	assets, err := EnsureTLSAssets()
	if err != nil {
		return nil, err
	}
	dialer := &net.Dialer{Timeout: 5 * time.Second}
	config := &tls.Config{
		RootCAs:            assets.rootCAs,
		ServerName:         "localhost",
		MinVersion:         tls.VersionTLS12,
		InsecureSkipVerify: false,
	}
	conn, err := tls.DialWithDialer(dialer, "tcp", addr, config)
	if err != nil {
		return nil, err
	}
	return &framedNetConn{conn: conn}, nil
}

func openWSStream(endpoint string, secure bool) (StreamConn, error) {
	assets, err := EnsureTLSAssets()
	if err != nil {
		return nil, err
	}
	dialer := websocket.Dialer{
		HandshakeTimeout: 5 * time.Second,
	}
	if secure {
		dialer.TLSClientConfig = &tls.Config{
			RootCAs:            assets.rootCAs,
			ServerName:         "localhost",
			MinVersion:         tls.VersionTLS12,
			InsecureSkipVerify: false,
		}
	}
	conn, _, err := dialer.Dial(endpoint, nil)
	if err != nil {
		return nil, err
	}
	return &framedWebSocketConn{conn: conn}, nil
}

func openPacketEndpoint(endpoint string) (PacketConn, error) {
	u, err := url.Parse(endpoint)
	if err != nil || u.Scheme == "" {
		return openPacketTCPStream(endpoint)
	}
	switch strings.ToLower(u.Scheme) {
	case "tcp":
		return openPacketTCPStream(endpoint)
	case "tls":
		return openPacketTLSStream(u.Host)
	case "ws":
		return openPacketWSStream(endpoint, false)
	case "wss":
		return openPacketWSStream(endpoint, true)
	default:
		return openPacketTCPStream(endpoint)
	}
}

func openPacketTCPStream(endpoint string) (PacketConn, error) {
	addr := endpoint
	if idx := strings.Index(addr, "://"); idx >= 0 {
		addr = addr[idx+3:]
	}
	if idx := strings.IndexByte(addr, '?'); idx >= 0 {
		addr = addr[:idx]
	}
	conn, err := net.DialTimeout("tcp", addr, 5*time.Second)
	if err != nil {
		return nil, err
	}
	return &packetNetConn{conn: conn}, nil
}

func openPacketTLSStream(addr string) (PacketConn, error) {
	assets, err := EnsureTLSAssets()
	if err != nil {
		return nil, err
	}
	dialer := &net.Dialer{Timeout: 5 * time.Second}
	config := &tls.Config{
		RootCAs:            assets.rootCAs,
		ServerName:         "localhost",
		MinVersion:         tls.VersionTLS12,
		InsecureSkipVerify: false,
	}
	conn, err := tls.DialWithDialer(dialer, "tcp", addr, config)
	if err != nil {
		return nil, err
	}
	return &packetNetConn{conn: conn}, nil
}

func openPacketWSStream(endpoint string, secure bool) (PacketConn, error) {
	assets, err := EnsureTLSAssets()
	if err != nil {
		return nil, err
	}
	dialer := websocket.Dialer{
		HandshakeTimeout: 5 * time.Second,
	}
	if secure {
		dialer.TLSClientConfig = &tls.Config{
			RootCAs:            assets.rootCAs,
			ServerName:         "localhost",
			MinVersion:         tls.VersionTLS12,
			InsecureSkipVerify: false,
		}
	}
	conn, _, err := dialer.Dial(endpoint, nil)
	if err != nil {
		return nil, err
	}
	return &packetWebSocketConn{conn: conn}, nil
}

type framedNetConn struct {
	conn       net.Conn
	pending    []byte
	pendingPos int
}

func (c *framedNetConn) Read(p []byte) (int, error) {
	if c.pendingPos >= len(c.pending) {
		if err := c.fill(); err != nil {
			return 0, err
		}
	}
	if len(c.pending) == 0 {
		return 0, io.EOF
	}
	n := copy(p, c.pending[c.pendingPos:])
	c.pendingPos += n
	return n, nil
}

func (c *framedNetConn) fill() error {
	var header [4]byte
	if _, err := io.ReadFull(c.conn, header[:]); err != nil {
		return err
	}
	size := binary.BigEndian.Uint32(header[:])
	if size == 0 {
		c.pending = c.pending[:0]
		c.pendingPos = 0
		return nil
	}
	buf := make([]byte, int(size))
	if _, err := io.ReadFull(c.conn, buf); err != nil {
		return err
	}
	c.pending = buf
	c.pendingPos = 0
	return nil
}

func (c *framedNetConn) Write(p []byte) (int, error) {
	frame := make([]byte, 4+len(p))
	binary.BigEndian.PutUint32(frame[:4], uint32(len(p)))
	copy(frame[4:], p)
	if err := writeFull(c.conn, frame); err != nil {
		return 0, err
	}
	return len(p), nil
}

func (c *framedNetConn) Close() error {
	return c.conn.Close()
}

func (c *framedNetConn) SetDeadline(t time.Time) error {
	return c.conn.SetDeadline(t)
}

type framedWebSocketConn struct {
	conn       *websocket.Conn
	pending    []byte
	pendingPos int
}

func (c *framedWebSocketConn) Read(p []byte) (int, error) {
	if c.pendingPos >= len(c.pending) {
		if err := c.fill(); err != nil {
			return 0, err
		}
	}
	if len(c.pending) == 0 {
		return 0, io.EOF
	}
	n := copy(p, c.pending[c.pendingPos:])
	c.pendingPos += n
	return n, nil
}

func (c *framedWebSocketConn) fill() error {
	_, data, err := c.conn.ReadMessage()
	if err != nil {
		return err
	}
	if len(data) < 4 {
		return fmt.Errorf("websocket frame too small")
	}
	size := binary.BigEndian.Uint32(data[:4])
	if int(size) > len(data)-4 {
		return fmt.Errorf("websocket frame length mismatch")
	}
	c.pending = append(c.pending[:0], data[4:4+int(size)]...)
	c.pendingPos = 0
	return nil
}

func (c *framedWebSocketConn) Write(p []byte) (int, error) {
	frame := make([]byte, 4+len(p))
	binary.BigEndian.PutUint32(frame[:4], uint32(len(p)))
	copy(frame[4:], p)
	if err := c.conn.WriteMessage(websocket.BinaryMessage, frame); err != nil {
		return 0, err
	}
	return len(p), nil
}

func (c *framedWebSocketConn) Close() error {
	_ = c.conn.WriteMessage(
		websocket.CloseMessage,
		websocket.FormatCloseMessage(websocket.CloseNormalClosure, ""),
	)
	return c.conn.Close()
}

func (c *framedWebSocketConn) SetDeadline(t time.Time) error {
	return c.conn.UnderlyingConn().SetDeadline(t)
}

type packetNetConn struct {
	conn       net.Conn
	pending    []byte
	pendingPos int
}

func (c *packetNetConn) Read(p []byte) (int, error) {
	if c.pendingPos >= len(c.pending) {
		if err := c.fill(); err != nil {
			return 0, err
		}
	}
	if len(c.pending) == 0 {
		return 0, io.EOF
	}
	n := copy(p, c.pending[c.pendingPos:])
	c.pendingPos += n
	return n, nil
}

func (c *packetNetConn) fill() error {
	var prefix [6]byte
	if _, err := io.ReadFull(c.conn, prefix[:]); err != nil {
		return err
	}
	headerSize := binary.BigEndian.Uint16(prefix[:2])
	bodySize := binary.BigEndian.Uint32(prefix[2:6])
	if headerSize > 0 {
		if _, err := io.CopyN(io.Discard, c.conn, int64(headerSize)); err != nil {
			return err
		}
	}
	if bodySize == 0 {
		c.pending = c.pending[:0]
		c.pendingPos = 0
		return nil
	}
	buf := make([]byte, int(bodySize))
	if _, err := io.ReadFull(c.conn, buf); err != nil {
		return err
	}
	c.pending = buf
	c.pendingPos = 0
	return nil
}

func (c *packetNetConn) Write(p []byte) (int, error) {
	frame := make([]byte, 6+len(p))
	binary.BigEndian.PutUint16(frame[:2], 0)
	binary.BigEndian.PutUint32(frame[2:6], uint32(len(p)))
	copy(frame[6:], p)
	if err := writeFull(c.conn, frame); err != nil {
		return 0, err
	}
	return len(p), nil
}

func (c *packetNetConn) Close() error {
	return c.conn.Close()
}

func (c *packetNetConn) SetDeadline(t time.Time) error {
	return c.conn.SetDeadline(t)
}

type packetWebSocketConn struct {
	conn       *websocket.Conn
	pending    []byte
	pendingPos int
}

func (c *packetWebSocketConn) Read(p []byte) (int, error) {
	if c.pendingPos >= len(c.pending) {
		if err := c.fill(); err != nil {
			return 0, err
		}
	}
	if len(c.pending) == 0 {
		return 0, io.EOF
	}
	n := copy(p, c.pending[c.pendingPos:])
	c.pendingPos += n
	return n, nil
}

func (c *packetWebSocketConn) fill() error {
	_, data, err := c.conn.ReadMessage()
	if err != nil {
		return err
	}
	if len(data) < 6 {
		return fmt.Errorf("websocket packet frame too small")
	}
	headerSize := binary.BigEndian.Uint16(data[:2])
	bodySize := binary.BigEndian.Uint32(data[2:6])
	offset := 6 + int(headerSize)
	if offset > len(data) {
		return fmt.Errorf("websocket packet header length mismatch")
	}
	if int(bodySize) > len(data)-offset {
		return fmt.Errorf("websocket packet body length mismatch")
	}
	c.pending = append(c.pending[:0], data[offset:offset+int(bodySize)]...)
	c.pendingPos = 0
	return nil
}

func (c *packetWebSocketConn) Write(p []byte) (int, error) {
	frame := make([]byte, 6+len(p))
	binary.BigEndian.PutUint16(frame[:2], 0)
	binary.BigEndian.PutUint32(frame[2:6], uint32(len(p)))
	copy(frame[6:], p)
	if err := c.conn.WriteMessage(websocket.BinaryMessage, frame); err != nil {
		return 0, err
	}
	return len(p), nil
}

func (c *packetWebSocketConn) Close() error {
	_ = c.conn.WriteMessage(
		websocket.CloseMessage,
		websocket.FormatCloseMessage(websocket.CloseNormalClosure, ""),
	)
	return c.conn.Close()
}

func (c *packetWebSocketConn) SetDeadline(t time.Time) error {
	return c.conn.UnderlyingConn().SetDeadline(t)
}

func writeFull(conn net.Conn, data []byte) error {
	for len(data) > 0 {
		n, err := conn.Write(data)
		if err != nil {
			return err
		}
		data = data[n:]
	}
	return nil
}
