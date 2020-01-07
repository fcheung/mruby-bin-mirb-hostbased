#include <unistd.h>
#include <mruby.h>


typedef enum {
  HOSTBASED_UNHANDLED = 0,
  HOSTBASED_HANDLED,
  HOSTBASED_REREAD,
} mirb_command_status;

void remote_eval(mrb_state *mrb, struct RProc * proc, int fd_port, int verbose);
mirb_command_status mirb_hostbased_command(char *ruby_code, char *last_code_line, size_t max_length, int fd_port, const char *port);
int init_host_based(const char *port, int noreset);
int mirb_reconnect(const char *port, int *fd);


