#include <zlink.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
static int request(void *dealer, int flags, uintptr_t context, zlink_completion_id_t *id) {
    zlink_msg_t body, tail;
    zlink_msg_init_size(&body, 65536);
    int rc = zlink_request_part(dealer, NULL, &body, flags, ZLINK_PART_MORE, 0, NULL, NULL);
    zlink_msg_close(&body);
    if (rc != ZLINK_SUBMIT_OK) return rc;
    zlink_msg_init_size(&tail, 0);
    rc = zlink_request_part(dealer, NULL, &tail, flags, ZLINK_PART_FINAL, 200, (void *)context, id);
    zlink_msg_close(&tail);
    return rc;
}
int main(void) {
    void *ctx=zlink_ctx_new();
    void *router=zlink_socket(ctx,ZLINK_SOCKET_ROUTER), *dealer=zlink_socket(ctx,ZLINK_SOCKET_DEALER);
    zlink_bind(router,"inproc://pass3-native-writable");zlink_connect(dealer,"inproc://pass3-native-writable");
    zlink_completion_id_t id=0;
    int rc=request(dealer,0,1,&id);
    printf("initial rc=%d errno=%d id=%llu\n",rc,zlink_errno(),(unsigned long long)id);
    if(rc!=ZLINK_SUBMIT_OK)return 2;
    for(int i=0;i<2;i++) {
        zlink_msg_t part; zlink_msg_init(&part);
        const zlink_routing_id_t *rid; zlink_reply_token_t reply; zlink_part_flag_t more;
        rc=zlink_router_recv_part(router,&rid,&reply,&part,&more,0);
        zlink_msg_close(&part); if(rc!=ZLINK_RECV_OK)return 3;
    }
    struct timespec a,b;clock_gettime(CLOCK_MONOTONIC,&a);
    int writable=0, rejected=0, nodata=0, other=0;
    for(int i=0;i<1000;i++) {
        id=0;rc=request(dealer,ZLINK_SEND_FLAGS_DONTWAIT,2,&id);int err=zlink_errno();
        if(i<3)printf("retry%d rc=%d errno=%d id=%llu\n",i,rc,err,(unsigned long long)id);
        if(rc!=ZLINK_SUBMIT_BACKPRESSURED || !id) {other++;break;}
        rejected++;
        zlink_completion_t c;memset(&c,0,sizeof(c));c.struct_size=sizeof(c);
        rc=zlink_completion_recv(dealer,&c,ZLINK_RECV_FLAGS_DONTWAIT);
        if(rc!=ZLINK_RECV_OK){nodata++;break;}
        if(c.kind==ZLINK_COMPLETION_WRITABLE && c.send_result==ZLINK_SEND_ADMITTED && c.completion_id==id)writable++;else other++;
        zlink_completion_close(&c);
    }
    clock_gettime(CLOCK_MONOTONIC,&b);
    printf("no reply or competing sender: rejected=%d immediate_writable=%d nodata=%d other=%d elapsed_ms=%.3f\n",rejected,writable,nodata,other,(b.tv_sec-a.tv_sec)*1000.0+(b.tv_nsec-a.tv_nsec)/1e6);
    zlink_close(dealer);zlink_close(router);zlink_ctx_term(ctx);
    return writable==1000 ? 0 : 4;
}
