#ifndef VAR_SERVER_H
#define VAR_SERVER_H

int var_server_init(const char *endpoint);
int var_server_run(void);
void var_server_stop(void);

#endif
