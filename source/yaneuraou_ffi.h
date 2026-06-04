#ifndef YANEURAOU_FFI_H_INCLUDED
#define YANEURAOU_FFI_H_INCLUDED

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*yaneuraou_recv_cb)(const char* line, void* user);

int  yaneuraou_init(int argc, char** argv);
void yaneuraou_set_recv(yaneuraou_recv_cb cb, void* user);
int  yaneuraou_send(const char* cmd);
void yaneuraou_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
