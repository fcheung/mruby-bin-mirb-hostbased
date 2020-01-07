#include "hostbased.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <termios.h>

#include <string.h>
#include <signal.h>

#include <mruby.h>
#include <mruby/dump.h>
#include <mruby/proc.h>

#define DPRINTF(...) if (verbose) printf(__VA_ARGS__)

int g_continue_view;

static void
sigint_handler(int sig)
{
  g_continue_view = 0;
}



ssize_t
read_target(int fd, char *buffer, int timeout){

  // non-blocking 1 byte read with timeout
  // timeout in roughly 10 mSec ticks (0 == infinite)
  ssize_t result = 0;
  while(0==result){
    result = read(fd, buffer, 1);
    if(0>=result){
      // normal with O_NONBLOCK
      result = 0;
      if ( 0 != timeout ){
        if ( 0 >= --timeout ) break;
      }
      usleep(10000);
    }
  }
  return result;
}

void
read_flush(int fd){

  // flush all serial input
  char c;
  usleep(1000);
  while( 0 < read(fd, &c, 1) )
    ;
}

int
wait_hello(const char *port , int *fd){

  // use ENQ/ACK polling to sync with the target
  const char ACK = 0x06;
  char c=0;
  const char ENQ = 0x05;
  int retry = 100;

  int send_enq = 1;

  while(0<retry--){
    c = ENQ;
    int ret = 0;
    if (send_enq){
      ret = write(*fd, &c, 1);
      
      //special handling for chipKIT Max32
      //we dont send ENQ for chipKIT Max32 because bootloader enter update mode once some data received..
      if ((ret == -1) || (errno == EAGAIN || errno == EWOULDBLOCK)){
        printf("  chipKIT detected. reopening port..\n");
        mirb_reconnect(port , fd);     //force board to reset
        send_enq = 0;             //don't send ENQ anymore.
      }
    }
    while ( 1 == read_target(*fd, &c, 20) ) {
      if (c == ACK) {
        break;
      }else{
        putc(c, stdout);
      }
    }
    if (c == ACK) break;
  }
  if (c != ACK){
    printf("sync error\n");
    return -1;
  }
  
  return 0;
}

int read_result(mrb_state *mrb, int fd, char **result_str, int *is_exeption){

  const char SOH = 0x01;          //Header for normal result
  const char SOH_EXCEPTION = 0x02;//Header for exception
  ssize_t read_size;
  char c;
  while(1){    
    read_size = read_target(fd, &c, 0);
    if (read_size != 1) goto read_error;
    if (c == SOH || c == SOH_EXCEPTION) break;

    //normal output from target
    putc(c, stdout);
  }

  *is_exeption = (c == SOH)? 0 : 1;

  unsigned char len_h;
  unsigned char len_l;
  read_size = read_target(fd, (char *)&len_h, 20);
  if (read_size != 1) goto read_error;
  read_size = read_target(fd, (char *)&len_l, 20);
  if (read_size != 1) goto read_error;

  char ack = '!';
  ssize_t written_size = write(fd, &ack, 1);
  if (written_size != 1) goto write_error;

  unsigned short len_to_read = ((unsigned short)len_h << 8) | len_l;

  unsigned short len_readed = 0;
  int i;
  *result_str = mrb_malloc(mrb, len_to_read+1);
  while(len_readed < len_to_read){
    for (i = 0 ; i < 100; i++){
      read_size = read_target(fd, *result_str+len_readed, 20);
      if (read_size != 1) goto read_error;
      len_readed++;
      if (len_readed == len_to_read){
        break;
      }
    }
    //send ack;
    ack = '#';
    written_size = write(fd, &ack, 1);
    if (written_size != 1) goto write_error;

  }
  (*result_str)[len_to_read]=0;
  return 0;

read_error:
  perror("read error\n");
  return -1;
write_error:
  perror("write error\n");
  return -1;
}

int write_bytecode(int fd, const void *buffer, int len, int verbose){

  ssize_t read_size;
  ssize_t written_size;

  unsigned char header[3];
  header[0] = verbose ? 0x02 : 0x01; //1:SOH 2:SOH with verbose
  header[1] = (unsigned char)(len >> 8);
  header[2] = (unsigned char)(len & 0xFF);

  char ack='?';
  int retry = 5;
  while ((ack != '!') && (0 < retry--)){
    read_flush(fd);
    (void)write(fd, header, 3);
    (void)read_target(fd, &ack, 20);
  }
  if ( '!' != ack ){
    printf("protocol error(first ack:%c)\n",ack);
    return -1;
  }

  unsigned short len_written = 0;
  while(len_written < len){
    int i=0;
    while(i < 100){
      written_size = write(fd, buffer + len_written,1);
      if((-1==written_size) && (EAGAIN==errno)) continue;// no i++
      else if (written_size != 1) {
        perror("write error\n");
        return -1;
      }
      i++;
      len_written++;
      if (len_written == len){
        break;
      }
    }
    ack = '?';
    read_size = read_target(fd, &ack, 20);
    if ( (read_size != 1) || ack != '#'){
      printf("protocol error(normal ack:%c)\n", ack);
      return -1;
    }
  }

  //OK all data sent.
  return 0;

}

int 
mirb_reconnect(const char *port, int *fd)
{
  /*ok here open serial port*/
  int fd_port = open(port, O_RDWR | O_NOCTTY | O_NONBLOCK);
  if (fd_port < 0){
    printf("failed to open port %s\n",port);
    perror("error:");
    return -1;
  }
  //http://irobot.csse.muroran-it.ac.jp/html-corner/robotMaker/elements/outlineSerialCommProgramming/
  struct termios oldtio, newtio;
  tcgetattr(fd_port, &oldtio);
  newtio = oldtio;
  cfsetspeed(&newtio, B9600);
  tcflush(fd_port, TCIFLUSH);
  tcsetattr(fd_port,TCSANOW, &newtio);

  *fd = fd_port;
  return 0;
}

int mirb_hostbased_command(char *ruby_code, char *last_code_line, size_t max_length, int fd_port, const char *port) {
    if ((strncmp(last_code_line,"#file",strlen("#file")) == 0) || 
            (strncmp(last_code_line,"#load",strlen("#load")) == 0)){
    char *filename = last_code_line + strlen("#file");

    //strip space
    while(filename[0] == ' ' || filename[0] == '\t' || filename[0] == '"'){
      filename++;
    }
    while(filename[strlen(filename)-1] == ' ' || filename[strlen(filename)-1] == '\t' ||
          filename[strlen(filename)-1] == '"'){
      filename[strlen(filename)-1] = '\0';
    }

    FILE *f = fopen(filename, "r");
    if (!f){
      printf("cannot open file:%s\n",filename);
      return 1;
    }
    char line[1024];
    while(fgets(line, 1024, f) != NULL){
      char c = line[0];
      int is_comment_line = 0;
      //
      while(TRUE){
        if(c == '#') {
          is_comment_line = 1;
          break;
        }else if (c == '\n') {
          break;
        }else if (c == ' ' || c == '\t'){
          c++;
          continue;
        }else{
          break;
        }
      }
      if (!is_comment_line) {
        if(strlen(ruby_code) + strlen(line) > max_length - 1){
          printf("file too large\n");
          return 1;
        }
        strlcat(ruby_code, line, max_length );
      }
    }
    fclose(f);
    
    //remove '\n' or spaces from last line to prevent code_block_open
    while(TRUE){
      char last_char = ruby_code[strlen(ruby_code)-1];
      if (last_char == '\n' || last_char == ' ' || last_char == '\t'){
        ruby_code[strlen(ruby_code)-1] = '\0';
        continue;
      }
      break;
    }
  }else if (strncmp(last_code_line,"#reconnect",strlen("#reconnect")) == 0){
    close(fd_port);
    printf("reconnecting to %s...", port);
    if(0 != mirb_reconnect(port, &fd_port)) {
      printf("\nfailed. Check connectivity.\n");
    }else{
      printf("\n");
    }
  }else if (strncmp(last_code_line, "#view", strlen("#view")) == 0){
    //view mode
    printf("...Entering view mode.. press Ctrl-C to back to REPL...\n");
    g_continue_view = 1;

    if (SIG_ERR == signal(SIGINT, sigint_handler)){
      printf("failed to set signal handler");
      return 1;
    }

    while(g_continue_view){
      char c;
      ssize_t ret = read(fd_port, &c, 1);
      if (ret == 1){
        putc(c,stdout);
        return 1;
      }else if (ret > 0){
        printf("ret = %zd\n", ret);
      }else{    //need strick check. currently assume EAGAIN or EWOULDBLOCK
        if ((errno != EAGAIN) && (errno != EWOULDBLOCK)){
          printf("oops, something bad happen");
          return 1;
        }
      }
    }
    printf("\n...get back to REPL\n");
    signal(SIGINT, SIG_DFL);  //restore default handler

    ruby_code[0] = '\0';
    last_code_line[0] = '\0';
  } else {
    return 0;
  }
  return 1;
}


void remote_eval(mrb_state *mrb, struct RProc * proc, int fd_port, int verbose){
  size_t bytecode_size;
  uint8_t *bytecode_data;

  int ret = mrb_dump_irep(mrb, proc->body.irep, 0, &bytecode_data, &bytecode_size);
  if (ret != MRB_DUMP_OK){
    printf("failed to dump bytecode. err = %d\n", ret);
  }else{
    ret = write_bytecode(fd_port, bytecode_data, bytecode_size, verbose);
    mrb_free(mrb, bytecode_data);

    if(ret){
      printf("failed to send bytecode.\n");
      printf("type #reconnect to reconnect to target without reset.\n");
      return;
    }
    else {
      char *result=NULL;
      int is_exception = 0;
      ret = read_result(mrb, fd_port, &result, &is_exception);
      if (ret != 0){
        if(result){
          mrb_free(mrb, result);
        }

        printf("failed to get result.\n");
        printf("type #reconnect to reconnect to target without reset.\n");
        return;
      }
      DPRINTF("(host:)receiving result from target...done. len=%zd\n",strlen(result));
      if (is_exception){
        printf("   %s\n",result);
      }else{
        printf(" => %s\n",result);
      }
      mrb_free(mrb, result);
    }
  }
}

int init_host_based(const char *port, int noreset){
    /*ok here open serial port*/
  int fd_port = 0;
  if (0 != mirb_reconnect(port, &fd_port)){
    return 0;
  }
  if (!noreset){
    printf("  waiting for target on %s...\n", port);
    fflush(stdout);
    int ret = wait_hello(port, &fd_port);
    if (ret != 0){
      printf("\nfailed to open communication with target.\n");
      return 0;
    }
  }else{
    printf("continue without reset. Note:local variables are not restored.\n");
  }
  printf("target is ready.\n");
  return fd_port;
}
