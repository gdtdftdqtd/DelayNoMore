#include "udp_session.hpp"
#include "base/ccMacros.h"
#include "cocos/platform/CCApplication.h"
#include "cocos/base/CCScheduler.h"
#include "cocos/scripting/js-bindings/jswrapper/SeApi.h"
#include "uv/uv.h"

uv_udp_t* udpSocket = NULL;
uv_thread_t recvTid;
uv_async_t uvLoopStopSig;
uv_loop_t* loop = NULL; // Only this loop is used for this simple PoC

int const maxPeerCnt = 10;
struct PeerAddr {
    struct sockaddr_in sockAddrIn;
    uint32_t authKey;
};
struct PeerAddr peerAddrList[maxPeerCnt];

uv_mutex_t sendLock, recvLock;

CHARC * SRV_IP = NULL;
int SRV_PORT = 0;

void _onRead(uv_udp_t* req, ssize_t nread, const uv_buf_t* buf, const struct sockaddr* addr, unsigned flags) {
    if (nread < 0) {
        CCLOGERROR("Read error %s", uv_err_name(nread));
        uv_close((uv_handle_t*)req, NULL);
        free(buf->base);
        return;
    }

    struct sockaddr_in* sockAddr = (struct sockaddr_in*)addr;
    char ip[64] = { 0 };
    uv_ip4_name(sockAddr, ip, sizeof ip);
    int port = sockAddr->sin_port;
    
    int const gameThreadMsgSize = 256;
    char* const gameThreadMsg = (char* const)malloc(gameThreadMsgSize);
    memset(gameThreadMsg, 0, gameThreadMsgSize);
    memcpy(gameThreadMsg, buf->base, nread);

    CCLOG("Recv %d bytes from %s:%d, converted to %d bytes for the JS callback", nread, ip, port, strlen(gameThreadMsg));
    free(buf->base);
    //uv_udp_recv_stop(req);
    
    cocos2d::Application::getInstance()->getScheduler()->performFunctionInCocosThread([=]() {
        // [WARNING] Use of the "ScriptEngine" is only allowed in "GameThread a.k.a. CocosThread"!
        se::Value onUdpMessageCb;
        se::ScriptEngine::getInstance()->getGlobalObject()->getProperty("onUdpMessage", &onUdpMessageCb);
        // [WARNING] Declaring "AutoHandleScope" is critical here, otherwise "onUdpMessageCb.toObject()" wouldn't be recognized as a function of the ScriptEngine!
        se::AutoHandleScope hs;
        se::ValueArray args = { se::Value(gameThreadMsg) };
        if (onUdpMessageCb.isObject() && onUdpMessageCb.toObject()->isFunction()) {
            // Temporarily assume that the "this" ptr within callback is NULL.
            bool ok = onUdpMessageCb.toObject()->call(args, NULL);
            if (!ok) {
                se::ScriptEngine::getInstance()->clearException();
            }
        }
        free(gameThreadMsg);
    });
}

static void _allocBuffer(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf) {
    (void)handle;
    buf->base = (char *)malloc(suggested_size);
    buf->len = suggested_size;
}

void diep(char* s) {
    perror(s);
    exit(1);
}
typedef struct client {
    int host;
    short port;
};

void _onUvStopSig(uv_async_t* handle) {
    uv_stop(loop);
    CCLOG("UDP recv loop is signaled to stop in UvThread");
}

void _onWalkCleanup(uv_handle_t* handle, void* data) {
    (void)data;
    uv_close(handle, NULL);
}

void startRecvLoop(void* arg) {
    uv_loop_t* l = (uv_loop_t*)arg;
    int uvRunRet1 = uv_run(l, UV_RUN_DEFAULT);
    CCLOG("UDP recv loop is ended in UvThread, uvRunRet1=%d", uvRunRet1);
    uv_walk(l, _onWalkCleanup, NULL);
    int uvRunRet2 = uv_run(l, UV_RUN_DEFAULT);

    int uvCloseRet = uv_loop_close(l);
    CCLOG("UDP recv loop is closed in UvThread, uvRunRet2=%d, uvCloseRet=%d", uvRunRet2, uvCloseRet);
}

bool DelayNoMore::UdpSession::openUdpSession(int port) {
    
    uv_mutex_init(&sendLock);
    uv_mutex_init(&recvLock);

    udpSocket = (uv_udp_t*)malloc(sizeof(uv_udp_t));
    struct sockaddr_in recv_addr;
    uv_ip4_addr("0.0.0.0", port, &recv_addr);
    uv_udp_bind(udpSocket, (struct sockaddr const*)&recv_addr, UV_UDP_REUSEADDR);
    
    CCLOG("About to open UDP session at port=%d...", port);
    loop = uv_loop_new();
    uv_udp_init(loop, udpSocket);
    uv_async_init(loop, &uvLoopStopSig, _onUvStopSig);
    uv_udp_recv_start(udpSocket, _allocBuffer, _onRead);

    uv_thread_create(&recvTid, startRecvLoop, loop);

    CCLOG("Finished opening UDP session at port=%d", port);

    return true;
}

bool DelayNoMore::UdpSession::closeUdpSession() { 
    CCLOG("About to close udp session and dealloc all resources...");
    
    for (int i = 0; i < maxPeerCnt; i++) {
        peerAddrList[i].authKey = -1; // hardcoded for now
        memset((char*)&peerAddrList[i].sockAddrIn, 0, sizeof(peerAddrList[i].sockAddrIn));
    }
    uv_async_send(&uvLoopStopSig); // The few if not only guaranteed thread safe utility of libuv :) See http://docs.libuv.org/en/v1.x/async.html#c.uv_async_send
    CCLOG("Signaling UvThread to end in GameThread...");

    uv_thread_join(&recvTid);

    free(udpSocket);
    free(loop);

    uv_mutex_destroy(&sendLock);
    uv_mutex_destroy(&recvLock);
    
    CCLOG("Closed udp session and dealloc all resources in GameThread...");

    return true;
}

void _onSend(uv_udp_send_t* req, int status) {
    free(req);
    if (status) {
        fprintf(stderr, "uv_udp_send_cb error: %s\n", uv_strerror(status));
    }
}

bool DelayNoMore::UdpSession::upsertPeerUdpAddr(int joinIndex, CHARC* const ip, int port, uint32_t authKey, int roomCapacity, int selfJoinIndex) {
    CCLOG("upsertPeerUdpAddr called by js for joinIndex=%d, ip=%s, port=%d, authKey=%lu; roomCapacity=%d, selfJoinIndex=%d.", joinIndex, ip, port, authKey, roomCapacity, selfJoinIndex);
    uv_ip4_addr(ip, port, &(peerAddrList[joinIndex - 1].sockAddrIn));
    peerAddrList[joinIndex - 1].authKey = authKey;

    // Punching between existing peer-pairs for Address/Port-restricted Cone NAT (not need for Full Cone NAT) 
    uv_mutex_lock(&sendLock);
    for (int i = 0; i < roomCapacity; i++) {
        if (i == selfJoinIndex - 1) continue;
        for (int j = 0; j < 10; j++) {
            uv_udp_send_t* req = (uv_udp_send_t*)malloc(sizeof(uv_udp_send_t));
            uv_buf_t sendBuffer = uv_buf_init("foobar", 6); // hardcoded for now
            uv_udp_send(req, udpSocket, &sendBuffer, 1, (struct sockaddr const*)&peerAddrList[i], _onSend);
        }
    }
    uv_mutex_unlock(&sendLock);

    return true;
}

bool DelayNoMore::UdpSession::punchToServer(CHARC* const srvIp, int const srvPort, BYTEC* const bytes) {
    /*
    [WARNING] The RAM space used for "bytes", either on stack or in heap, is preallocatedand managed by the caller.
    
    Moreover, there's no need to lock on "bytes". Only "udpSocket" is possibly accessed by multiple threads.
    */
    SRV_IP = srvIp;
    SRV_PORT = srvPort;

    uv_udp_send_t* req = (uv_udp_send_t*)malloc(sizeof(uv_udp_send_t));
    uv_buf_t sendBuffer = uv_buf_init(bytes, strlen(bytes)); 
    struct sockaddr_in destAddr;
    
    uv_ip4_addr(SRV_IP, SRV_PORT, &destAddr);
    uv_mutex_lock(&sendLock);
    uv_udp_send(req, udpSocket, &sendBuffer, 1, (struct sockaddr const*)&destAddr, _onSend);
    uv_mutex_unlock(&sendLock);
    
    return true;
}
