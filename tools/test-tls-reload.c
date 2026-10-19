/* Run on camera with its existing cert/key; never changes those files. */
#undef NDEBUG
#include <assert.h>
#include "../camera/src/tls.c"
int main(int argc,char **argv) {
    assert(argc==3);assert(tls_init(argv[1],argv[2])==0);
    struct tlsconn *old=tls_accept(-1);assert(old&&active->refs==2);
    struct tls_config *prior=active;
    assert(tls_init(argv[1],argv[2])==0&&active!=prior);
    assert(old->config==prior&&prior->refs==1);
    struct tls_config *good=active;
    assert(tls_init("/tmp/plug2view-no-such-cert",argv[2])==-1&&active==good&&tls_ready());
    assert(tls_init(argv[1],argv[1])==-1&&active==good);
    struct tlsconn *fresh=tls_accept(-1);assert(fresh&&fresh->config==good);
    assert(br_ssl_engine_current_state(&old->cc.eng)&BR_SSL_RECVREC);
    tls_free(old);tls_free(fresh);assert(active->refs==1);
    puts("PASS: certificate reload keeps existing connections; invalid reload retains good config");return 0;
}
