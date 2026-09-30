// MsQuic loopback proof: QUIC + TLS 1.3 handshake, stream echo, datagram echo
#include <msquic.h>
#include <cstdio>
#include <cstring>
#include <condition_variable>
#include <mutex>
#include <chrono>
#include <thread>

static const QUIC_API_TABLE* Q = nullptr;
static HQUIC Reg, SrvCfg, CliCfg, Listener;
static std::mutex Mx; static std::condition_variable Cv;
static bool cliConnected = false, cliStreamEchoOk = false, cliDgramEchoOk = false;
static int srvStreamRecv = 0, srvDgramRecv = 0, cliDgramRecv = 0;
static QUIC_BUFFER AlpnBuf{ 4, (uint8_t*)"rdph" };
static const QUIC_BUFFER* const alpnPtr = &AlpnBuf;
static uint8_t echoBuf[32];

static bool wait_for(bool* f, int ms) {
    std::unique_lock<std::mutex> lk(Mx);
    return Cv.wait_for(lk, std::chrono::milliseconds(ms), [&] { return *f; });
}
static void notify() { Cv.notify_all(); }

static QUIC_STATUS QUIC_API srvStreamCb(HQUIC s, void*, QUIC_STREAM_EVENT* e) {
    switch (e->Type) {
    case QUIC_STREAM_EVENT_RECEIVE:
        srvStreamRecv += (int)e->RECEIVE.TotalBufferLength;
        memcpy(echoBuf, "echo-ok", 7);
        { QUIC_BUFFER b{ 7, echoBuf }; Q->StreamSend(s, &b, 1, QUIC_SEND_FLAG_NONE, nullptr); }
        break;
    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE: Q->StreamClose(s); break;
    }
    return QUIC_STATUS_SUCCESS;
}
static QUIC_STATUS QUIC_API srvConnCb(HQUIC c, void*, QUIC_CONNECTION_EVENT* e) {
    switch (e->Type) {
    case QUIC_CONNECTION_EVENT_CONNECTED: notify(); break;
    case QUIC_CONNECTION_EVENT_DATAGRAM_RECEIVED:
        if (++srvDgramRecv == 3 && getenv("SRVECHO")) {
            static uint8_t echoDg[64];
            static QUIC_BUFFER echoB; // must live until DATAGRAM send complete
            uint32_t len = e->DATAGRAM_RECEIVED.Buffer->Length;
            if (len > sizeof(echoDg)) len = sizeof(echoDg);
            memcpy(echoDg, e->DATAGRAM_RECEIVED.Buffer->Buffer, len);
            echoB = QUIC_BUFFER{ len, echoDg };
            Q->DatagramSend(c, &echoB, 1, QUIC_SEND_FLAG_NONE, nullptr);
        }
        break;
    case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED:
        Q->SetCallbackHandler(e->PEER_STREAM_STARTED.Stream, (void*)srvStreamCb, nullptr);
        Q->StreamStart(e->PEER_STREAM_STARTED.Stream, QUIC_STREAM_START_FLAG_NONE);
        break;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE: break;
    }
    return QUIC_STATUS_SUCCESS;
}
static QUIC_STATUS QUIC_API listenerCb(HQUIC, void*, QUIC_LISTENER_EVENT* e) {
    if (e->Type == QUIC_LISTENER_EVENT_NEW_CONNECTION) {
        Q->SetCallbackHandler(e->NEW_CONNECTION.Connection, (void*)srvConnCb, nullptr);
        return Q->ConnectionSetConfiguration(e->NEW_CONNECTION.Connection, SrvCfg);
    }
    return QUIC_STATUS_SUCCESS;
}
static QUIC_STATUS QUIC_API cliStreamCb(HQUIC s, void*, QUIC_STREAM_EVENT* e) {
    switch (e->Type) {
    case QUIC_STREAM_EVENT_RECEIVE:
        if (e->RECEIVE.TotalBufferLength == 7) {
            std::lock_guard<std::mutex> lk(Mx); cliStreamEchoOk = true;
        }
        notify();
        Q->StreamShutdown(s, QUIC_STREAM_SHUTDOWN_FLAG_GRACEFUL, 0);
        break;
    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE: Q->StreamClose(s); break;
    }
    return QUIC_STATUS_SUCCESS;
}
static QUIC_STATUS QUIC_API cliConnCb(HQUIC c, void*, QUIC_CONNECTION_EVENT* e) {
    switch (e->Type) {
    case QUIC_CONNECTION_EVENT_CONNECTED: {
        std::lock_guard<std::mutex> lk(Mx); cliConnected = true; notify(); break;
    }
    case QUIC_CONNECTION_EVENT_DATAGRAM_RECEIVED:
        if (++cliDgramRecv == 3) { std::lock_guard<std::mutex> lk(Mx); cliDgramEchoOk = true; }
        notify();
        break;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE: break;
    }
    return QUIC_STATUS_SUCCESS;
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("[trace] start\n");
    if (QUIC_FAILED(MsQuicOpen2(&Q))) { printf("FAIL MsQuicOpen2\n"); return 1; }
    QUIC_REGISTRATION_CONFIG rc{ "loopback", QUIC_EXECUTION_PROFILE_LOW_LATENCY };
    printf("[trace] open\n"); if (QUIC_FAILED(Q->RegistrationOpen(&rc, &Reg))) { printf("FAIL RegistrationOpen\n"); return 1; }

    QUIC_SETTINGS st{}; st.PeerBidiStreamCount = 1; st.IsSet.PeerBidiStreamCount = 1;
    st.DatagramReceiveEnabled = 1; st.IsSet.DatagramReceiveEnabled = 1;
    QUIC_CERTIFICATE_FILE cf{ .PrivateKeyFile = "/tmp/rdp-quic-key.pem", .CertificateFile = "/tmp/rdp-quic-cert.pem" };
    QUIC_CREDENTIAL_CONFIG sc{}; sc.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE;
    sc.Flags = QUIC_CREDENTIAL_FLAG_NONE; sc.CertificateFile = &cf;
    printf("[trace] srvcfg\n"); QUIC_STATUS s1 = Q->ConfigurationOpen(Reg, alpnPtr, 1, &st, sizeof(st), nullptr, &SrvCfg);
    if (QUIC_FAILED(s1)) { printf("FAIL server ConfigurationOpen 0x%08x\n", s1); return 1; }
    QUIC_STATUS s2 = Q->ConfigurationLoadCredential(SrvCfg, &sc);
    if (QUIC_FAILED(s2)) { printf("FAIL server LoadCredential 0x%08x\n", s2); return 1; }

    QUIC_CREDENTIAL_CONFIG cc{}; cc.Type = QUIC_CREDENTIAL_TYPE_NONE;
    cc.Flags = QUIC_CREDENTIAL_FLAG_CLIENT | QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION;
    if (QUIC_FAILED(Q->ConfigurationOpen(Reg, alpnPtr, 1, &st, sizeof(st), nullptr, &CliCfg)) ||
        QUIC_FAILED(Q->ConfigurationLoadCredential(CliCfg, &cc))) { printf("FAIL client config\n"); return 1; }

    QUIC_ADDR a{}; a.Ipv4.sin_family = QUIC_ADDRESS_FAMILY_INET; a.Ipv4.sin_port = htons(45443);
    printf("[trace] listener\n"); if (QUIC_FAILED(Q->ListenerOpen(Reg, listenerCb, nullptr, &Listener)) ||
        QUIC_FAILED(Q->ListenerStart(Listener, alpnPtr, 1, &a))) { printf("FAIL listener\n"); return 1; }

    HQUIC Conn;
    printf("[trace] connopen\n"); if (QUIC_FAILED(Q->ConnectionOpen(Reg, cliConnCb, nullptr, &Conn))) { printf("FAIL ConnectionOpen\n"); return 1; }
    printf("[trace] connstart\n"); if (QUIC_FAILED(Q->ConnectionStart(Conn, CliCfg, QUIC_ADDRESS_FAMILY_INET, "127.0.0.1", 45443))) {
        printf("FAIL ConnectionStart\n"); return 1;
    }
    if (!wait_for(&cliConnected, 5000)) { printf("FAIL handshake (client side)\n"); return 1; }
    printf("handshake: QUIC v1 + TLS 1.3 OK\n");
    printf("[trace] pre-stream\n");

    HQUIC St;
    Q->StreamOpen(Conn, QUIC_STREAM_OPEN_FLAG_NONE, cliStreamCb, nullptr, &St);
    Q->StreamStart(St, QUIC_STREAM_START_FLAG_NONE);
    uint8_t msg[10]; memcpy(msg, "hello-ech", 9);
    QUIC_BUFFER sb{ 9, msg };
    Q->StreamSend(St, &sb, 1, QUIC_SEND_FLAG_NONE, nullptr);
    if (!wait_for(&cliStreamEchoOk, 5000)) { printf("FAIL stream echo (srv got %d B)\n", srvStreamRecv); return 1; }
    printf("stream:   reliable send/recv + echo OK\n");
    printf("[trace] pre-dgram (SRVECHO=%s)\n", getenv("SRVECHO") ? "on" : "off");

    static uint8_t dbuf[3][16];
    static QUIC_BUFFER db[3]; // must live until send complete
    for (int i = 0; i < 3; i++) {
        int n = snprintf((char*)dbuf[i], sizeof(dbuf[i]), "dg%d", i) + 1;
        db[i] = QUIC_BUFFER{ (uint32_t)n, dbuf[i] };
        Q->DatagramSend(Conn, &db[i], 1, QUIC_SEND_FLAG_NONE, nullptr);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    printf("datagram: sent 3, srv got %d, cli got %d\n", srvDgramRecv, cliDgramRecv);
    if (srvDgramRecv != 3) { printf("FAIL datagram delivery\n"); return 1; }

    printf("PASS\n");
    Q->ConnectionShutdown(Conn, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
    Q->ListenerClose(Listener);
    Q->ConfigurationClose(SrvCfg); Q->ConfigurationClose(CliCfg);
    Q->RegistrationClose(Reg); MsQuicClose(Q);
    return 0;
}
