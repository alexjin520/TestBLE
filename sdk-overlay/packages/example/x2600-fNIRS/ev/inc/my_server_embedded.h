#ifndef MY_SERVER_EMBEDDED_H_
#define MY_SERVER_EMBEDDED_H_

int my_server_embedded_main(int argc, char *argv[]);
int my_server_embedded_start(void);
int my_server_embedded_get_listen_fd(void);
int my_server_embedded_get_mainloop_fd(void);
int my_server_embedded_accept(void);
int my_server_embedded_dispatch(void);
void my_server_embedded_stop(void);

#endif
