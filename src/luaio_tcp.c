/* Copyright © 2015 coord.cn. All rights reserved.
 * @author: QianYe(coordcn@163.com)
 * @license: MIT license
 * @overview: 
 */

#include "luaio.h"
#include "luaio_init.h"
#include "luaio_timer.h"
#include "luaio_check_data.h"

typedef struct {
  size_t          type;
  uint64_t        timeout;
  uv_timer_t      *timer;
  lua_State       *thread;
  lua_State       *current_thread;
  luaio_buffer_t  *read_buffer;
  uv_tcp_t        handle;
  int             thread_ref;
  int             onconnect_ref;
} luaio_tcp_socket_t;

typedef struct {
  lua_State       *current_thread;
  uv_timer_t      *timer;
  uv_connect_t    req;
  int             timed_out;
} luaio_tcp_connect_req_t;

typedef struct {
  lua_State       *current_thread;
  uv_timer_t      *timer;
  size_t          bytes;
  int             write_data_ref;
  int             timed_out;
  uv_write_t      req;
} luaio_tcp_write_req_t;

static char luaio_tcp_socket_metatable_key;

#define luaio_tcp_check_socket(L, name) \
  luaio_tcp_socket_t *socket = lua_touserdata(L, 1); \
  if (socket == NULL || socket->type != LUAIO_TYPE_SOCKET) { \
    return luaL_argerror(L, 1, "socket:"#name" error: socket must be [userdata](socket)\n"); \
  }

/*local socket = tcp.new([true])*/
static int luaio_tcp_socket_new(lua_State *L) {
  int ref_thread = lua_toboolean(L, 1);

  luaio_tcp_socket_t *socket = lua_newuserdata(L, sizeof(luaio_tcp_socket_t));
  if (socket == NULL) {
    lua_pushnil(L);
    return 1;
  }

  uv_loop_t *loop = uv_default_loop();
  uv_tcp_init(loop, &socket->handle);

  socket->type = LUAIO_TYPE_SOCKET;
  socket->thread = L;
  socket->current_thread = L;
  socket->read_buffer = NULL;
  socket->timer = NULL;
  socket->timeout = 0;

  if (ref_thread) {
    lua_pushthread(L);
    socket->thread_ref = luaL_ref(L, LUA_REGISTRYINDEX);
  } else {
    socket->thread_ref = LUA_NOREF;
  }

  socket->onconnect_ref = LUA_NOREF;

  lua_pushlightuserdata(L, &luaio_tcp_socket_metatable_key);
  lua_rawget(L, LUA_REGISTRYINDEX);
  lua_setmetatable(L, -2);
  return 1;
}

#define luaio_tcp_check_port_and_host(L, name) \
  int port = luaL_checkinteger(L, 2); \
  if (port < 0 || port > 65535) { \
    return luaL_argerror(L, 2, "socket:"#name" error: port must be [0, 65535]\n"); \
  } \
  \
  struct sockaddr *addr; \
  struct sockaddr_in addr4; \
  struct sockaddr_in6 addr6; \
  const char *host = luaL_checkstring(L, 3); \
  if (uv_ip4_addr(host, port, &addr4) == 0) { \
    addr = (struct sockaddr*)(&addr4); \
  } else if (uv_ip6_addr(host, port, &addr6) == 0) { \
    addr = (struct sockaddr*)(&addr6); \
  } else { \
    return luaL_argerror(L, 3, "socket:"#name" error: host is not a IP address\n"); \
  }

/*local err = socket:bind(port, host, tcp_reuseport)*/
static int luaio_tcp_socket_bind(lua_State *L) {
  luaio_tcp_check_socket(L, bind(port, host, tcp_reuseport));
  luaio_tcp_check_port_and_host(L, bind(port, host, tcp_reuseport));

  /*tcp_reuseport*/
  int tcp_reuseport = lua_toboolean(L, 4);
  /*uv.h +521 src/unix/tcp.c +63 +81 +82*/
  int err = uv_tcp_bind(&socket->handle, addr, 0, tcp_reuseport);

  lua_pushinteger(L, err);
  return 1;
}

static void luaio_tcp_server_onconnect(uv_stream_t *handle, int status) {
  if (status ) {
    fprintf(stderr, "server onconnect error: %s\n", uv_strerror(status));
    return;
  }

  luaio_tcp_socket_t* server = container_of(handle, luaio_tcp_socket_t, handle);
  lua_State *L = server->thread;
  lua_State *co = lua_newthread(L);
  int thread_ref = luaL_ref(L, LUA_REGISTRYINDEX);

  /*onconnect*/
  lua_rawgeti(co, LUA_REGISTRYINDEX, server->onconnect_ref);

  luaio_tcp_socket_t *socket = lua_newuserdata(co, sizeof(luaio_tcp_socket_t));
  if (socket == NULL) {
    luaL_unref(L, LUA_REGISTRYINDEX, thread_ref);
    fprintf(stderr, "server onconnect error: no memory for new connection\n");
    return;
  }

  uv_loop_t *loop = uv_default_loop();
  uv_tcp_t *client_handle = &socket->handle;
  uv_tcp_init(loop, client_handle);
  int err = uv_accept(handle, (uv_stream_t*)client_handle);
  if (err) {
    luaL_unref(L, LUA_REGISTRYINDEX, thread_ref);
    uv_close((uv_handle_t*)(client_handle), NULL);
    fprintf(stderr, "server onconnect error: %s\n", uv_strerror(err));
    return;
  }

  socket->type = LUAIO_TYPE_SOCKET;
  socket->thread = co;
  socket->current_thread = co;
  socket->read_buffer = NULL;
  socket->timer = NULL;
  socket->timeout = server->timeout;
  socket->onconnect_ref = LUA_NOREF;
  socket->thread_ref = LUA_NOREF;

  lua_pushlightuserdata(co, &luaio_tcp_socket_metatable_key);
  lua_rawget(co, LUA_REGISTRYINDEX);
  lua_setmetatable(co, -2);

  luaio_resume(co, 1);
}

/*local err = socket:listen(onconnect, tcp_backlog)*/
static int luaio_tcp_socket_listen(lua_State *L) {
  luaio_tcp_check_socket(L, listen(onconnect, tcp_backlog));

  /*onconnect*/
  if (lua_type(L, 2) != LUA_TFUNCTION) {
    return luaL_argerror(L, 2, "socket:listen(onconnect, tcp_backlog) error: onconnect must be [function]\n"); 
  }
  lua_pushvalue(L, 2);
  socket->onconnect_ref = luaL_ref(L, LUA_REGISTRYINDEX);

  int tcp_backlog = luaL_checkinteger(L, 3);
  int err = uv_listen((uv_stream_t*)(&socket->handle), tcp_backlog, luaio_tcp_server_onconnect);

  lua_pushinteger(L, err);
  return 1;
}

static void luaio_tcp_socket_connect_timeout(uv_timer_t *handle) {
  luaio_tcp_connect_req_t *luaio_req = handle->data;
  lua_State *L = luaio_req->current_thread;

  luaio_timer_free(handle);
  luaio_req->timer = NULL;
  luaio_req->timed_out = 1;

  lua_pushinteger(L, UV_ETIMEDOUT);
  luaio_resume(L, 1);
}

static void luaio_tcp_socket_onconnect(uv_connect_t *req, int status) {
  luaio_tcp_connect_req_t *luaio_req = container_of(req, luaio_tcp_connect_req_t, req);
  lua_State *L = luaio_req->current_thread;

  uv_timer_t *timer = luaio_req->timer;
  if (timer != NULL) {
    uv_timer_stop(timer);
    luaio_timer_free(timer);
  }

  int timed_out = luaio_req->timed_out;
  luaio_pfree(luaio_req);
  if (timed_out) return;

  lua_pushinteger(L, status);
  luaio_resume(L, 1);
}

/*local err = socket:connect(port, host)*/
static int luaio_tcp_socket_connect(lua_State *L) {
  luaio_tcp_check_socket(L, connect(port, host));
  luaio_tcp_check_port_and_host(L, connect(port, host));

  uint64_t timeout = socket->timeout;
  uv_timer_t *timer = NULL;
  if (timeout != 0) {
    timer = luaio_timer_alloc();
    if (timer == NULL) {
      lua_pushinteger(L, UV_ENOMEM);
      return 1;
    }

    uv_timer_start(timer, 
                   luaio_tcp_socket_connect_timeout, 
                   timeout, 
                   0);
  }


  luaio_tcp_connect_req_t *luaio_req = luaio_palloc(sizeof(luaio_tcp_connect_req_t));
  if (luaio_req == NULL) {
    if (timer != NULL) {
      uv_timer_stop(timer);
      luaio_timer_free(timer);
    }
    
    lua_pushinteger(L, UV_ENOMEM);
    return 1;
  }

  int err = uv_tcp_connect(&luaio_req->req, 
                           &socket->handle,
                           addr,
                           luaio_tcp_socket_onconnect);
  if (err) {
    if (timer != NULL) {
      uv_timer_stop(timer);
      luaio_timer_free(timer);
    }
   
    luaio_pfree(luaio_req);
    lua_pushinteger(L, err);
    return 1;
  }

  if (timer != NULL) {
    timer->data = luaio_req;
  }

  luaio_req->current_thread = L;
  luaio_req->timer = timer;
  luaio_req->timed_out = 0;

  return lua_yield(L, 0);
}

/*local fd = socket:fd()*/
static int luaio_tcp_socket_fd(lua_State *L) {
  luaio_tcp_check_socket(L, fd());

  /*uv.h +[74-71] src/unix/internal.h -[244-249]*/
  lua_pushinteger(L, uv__stream_fd(&socket->handle));
  return 1;
}

/*socket:set_read_buffer(buffer)*/
static int luaio_tcp_socket_set_read_buffer(lua_State *L) {
  luaio_tcp_check_socket(L, read(buffer));

  luaio_buffer_t *buffer = lua_touserdata(L, 2);
  if (buffer == NULL || buffer->type != LUAIO_TYPE_READ_BUFFER) {
    return luaL_argerror(L, 2, "socket:setReadBuffer(buffer) error: buffer must be [ReadBuffer]\n");
  }

  socket->read_buffer = buffer;
  return 0;
}

static void luaio_tcp_socket_read_timeout(uv_timer_t *handle) {
  luaio_tcp_socket_t *socket = handle->data;
  lua_State *L = socket->current_thread;

  uv_read_stop((uv_stream_t*)(&socket->handle));
  luaio_timer_free(handle);

  lua_pushinteger(L, UV_ETIMEDOUT);
  luaio_resume(L, 1);
}

static void luaio_tcp_socket_onalloc(uv_handle_t *handle, 
                                     size_t suggested_size, 
                                     uv_buf_t *buf) {
  luaio_tcp_socket_t *socket = container_of(handle, luaio_tcp_socket_t, handle);
  lua_State* L = socket->current_thread;

  luaio_buffer_t *buffer = socket->read_buffer;
  if (buffer->capacity == 0) {
    size_t buffer_size = buffer->size;
    char *start = luaio_palloc(buffer_size);
    if (start == NULL) {
      uv_read_stop((uv_stream_t*)(&socket->handle));

      uv_timer_t *timer = socket->timer;
      if (timer != NULL) {
        uv_timer_stop(timer);
        luaio_timer_free(timer);
      }

      lua_pushinteger(L, UV_ENOMEM);
      luaio_resume(L, 1);
      return;
    }

    size_t capacity = luaio_pmemory_get_capacity(start);
    buffer->capacity = capacity;
    buffer->start = start;
    buffer->read_pos = start;
    /*buffer->parse_pos = start;*/
    buffer->write_pos = start;
    buffer->end = start + capacity;
  }

  char *write_pos = buffer->write_pos;
  buf->base = write_pos;
  buf->len = buffer->end - write_pos;
}

static void luaio_tcp_socket_onread(uv_stream_t *handle, 
                                    ssize_t nread, 
                                    const uv_buf_t* buf) {
  if (nread == 0) return;
  
  luaio_tcp_socket_t *socket = container_of(handle, luaio_tcp_socket_t, handle);
  lua_State* L = socket->current_thread;

  uv_read_stop((uv_stream_t*)(&socket->handle));

  uv_timer_t *timer = socket->timer;
  if (timer != NULL) {
    uv_timer_stop(timer);
    luaio_timer_free(timer);
  }

  if (nread > 0) {
    socket->read_buffer->write_pos += nread;
  }

  lua_pushinteger(L, nread);
  luaio_resume(L, 1);
}

/*local ret = socket:read()*/
static int luaio_tcp_socket_read(lua_State *L) {
  luaio_tcp_check_socket(L, read(buffer));

  if (socket->read_buffer == NULL) {
    return luaL_error(L, "socket:read() error: no read buffer, please set a read buffer.\n");
  }

  uint64_t timeout = socket->timeout;
  uv_timer_t *timer = NULL;
  if (timeout != 0) {
    timer = luaio_timer_alloc();
    if (timer == NULL) {
      lua_pushinteger(L, UV_ENOMEM);
      return 1;
    }

    uv_timer_start(timer, 
                   luaio_tcp_socket_read_timeout, 
                   timeout, 
                   0);
  }

  int err = uv_read_start((uv_stream_t*)(&socket->handle), 
                          luaio_tcp_socket_onalloc, 
                          luaio_tcp_socket_onread);
  if (err) {
    if (timer != NULL) {
      uv_timer_stop(timer);
      luaio_timer_free(timer);
    }
    
    lua_pushinteger(L, err);
    return 1;
  }

  socket->timer = timer;
  socket->current_thread = L;

  if (timer != NULL) {
    timer->data = socket;
  }

  return lua_yield(L, 0);
}

static int luaio_tcp_socket_try_write(uv_stream_t *handle, 
                                      uv_buf_t **bufs, 
                                      size_t *count, 
                                      size_t *written_bytes) {
  uv_buf_t *vbufs = *bufs;
  size_t vcount = *count;

  int err = uv_try_write(handle, vbufs, vcount);
  if (err == UV_ENOSYS || err == UV_EAGAIN) {
    return 0;
  }

  if (err < 0) {
    return err;
  }

  *written_bytes = err;
  size_t written = err;
  for (; written != 0 && vcount > 0; vbufs++, vcount--) {
    if (vbufs[0].len > written) {
      vbufs[0].base += written;
      vbufs[0].len -= written;
      written = 0;
      break;
    } else {
      written -= vbufs[0].len;
    }
  }

  *bufs = vbufs;
  *count = vcount;

  return 0;
}

static void luaio_tcp_socket_write_timeout(uv_timer_t *handle) {
  luaio_tcp_write_req_t *luaio_req = handle->data;
  lua_State *L = luaio_req->current_thread;
 
  int write_data_ref = luaio_req->write_data_ref;
  if (write_data_ref != LUA_NOREF) {
    luaL_unref(L, LUA_REGISTRYINDEX, write_data_ref);
  }

  luaio_timer_free(handle);
  luaio_req->timer = NULL;
  luaio_req->timed_out = 1;

  lua_pushinteger(L, 0);
  lua_pushinteger(L, UV_ETIMEDOUT);
  luaio_resume(L, 2);
}

static void luaio_tcp_socket_after_write(uv_write_t *req, int status) {
  luaio_tcp_write_req_t *luaio_req = container_of(req, luaio_tcp_write_req_t, req);
  lua_State* L = luaio_req->current_thread;
 
  uv_timer_t *timer = luaio_req->timer;
  if (timer != NULL) {
    uv_timer_stop(timer);
    luaio_timer_free(timer);
  }

  int write_data_ref = luaio_req->write_data_ref;
  if (write_data_ref != LUA_NOREF) {
    luaL_unref(L, LUA_REGISTRYINDEX, write_data_ref);
  }

  size_t bytes = luaio_req->bytes;
  int timed_out = luaio_req->timed_out;
  luaio_pfree(luaio_req);
  if (timed_out) return;

  if (status != 0) {
    lua_pushinteger(L, 0);
  } else {
    lua_pushinteger(L, bytes);
  }

  lua_pushinteger(L, status);
  luaio_resume(L, 2);
}

/*local bytes, err = socket:write(data)*/
static int luaio_tcp_socket_write(lua_State *L) {
  luaio_tcp_check_socket(L, write(data));
  /*common.h*/
  luaio_check_data(L, 2, socket:write(data));

  size_t written = 0;
  size_t vcount = count;
  uv_stream_t *stream_handle = (uv_stream_t*)(&socket->handle);
  int err = luaio_tcp_socket_try_write(stream_handle, 
                                       &bufs, 
                                       &vcount, 
                                       &written);
  if (err) {
    if (tmp != NULL) {
      luaio_stack_buffer_free(&stack_buf);
    }

    lua_pushinteger(L, 0);
    lua_pushinteger(L, err);
    return 2;
  }

  /*uv_try_write send all data*/
  if (vcount == 0) {
    if (tmp != NULL) {
      luaio_stack_buffer_free(&stack_buf);
    }

    lua_pushinteger(L, written);
    lua_pushinteger(L, 0);
    return 2;
  }

  uint64_t timeout = socket->timeout;
  uv_timer_t *timer = NULL;
  if (timeout != 0) {
    timer = luaio_timer_alloc();
    if (timer == NULL) {
      if (tmp != NULL) {
        luaio_stack_buffer_free(&stack_buf);
      }

      lua_pushinteger(L, written);
      lua_pushinteger(L, UV_ENOMEM);
      return 2;
    }

    uv_timer_start(timer, 
                   luaio_tcp_socket_write_timeout, 
                   timeout, 
                   0);
  }

  luaio_tcp_write_req_t *luaio_req = luaio_palloc(sizeof(luaio_tcp_write_req_t));
  if (luaio_req == NULL) {
    if (tmp != NULL) {
      luaio_stack_buffer_free(&stack_buf);
    }

    if (timer != NULL) {
      uv_timer_stop(timer);
      luaio_timer_free(timer);
    }

    lua_pushinteger(L, written);
    lua_pushinteger(L, UV_ENOMEM);
    return 2;
  }

  err = uv_write2(&luaio_req->req, 
                  stream_handle, 
                  bufs, 
                  vcount, 
                  NULL, 
                  luaio_tcp_socket_after_write);
  if (err) {
    if (tmp != NULL) {
      luaio_stack_buffer_free(&stack_buf);
    }

    if (timer != NULL) {
      uv_timer_stop(timer);
      luaio_timer_free(timer);
    }

    luaio_pfree(luaio_req);
    lua_pushinteger(L, written);
    lua_pushinteger(L, err);
    return 2;
  }

  lua_pushvalue(L, 2);
  luaio_req->write_data_ref = luaL_ref(L, LUA_REGISTRYINDEX);
  luaio_req->current_thread = L;
  luaio_req->timer = timer;
  luaio_req->timed_out = 0;
  luaio_req->bytes = bytes;

  if (timer != NULL) {
    timer->data = luaio_req;
  }

  if (tmp != NULL) {
    luaio_stack_buffer_free(&stack_buf);
  }

  return lua_yield(L, 0);
}

static void luaio_tcp_socket_write_async_timeout(uv_timer_t *handle) {
  luaio_tcp_write_req_t *luaio_req = handle->data;
  lua_State *L = luaio_get_main_thread();
 
  int write_data_ref = luaio_req->write_data_ref;
  if (write_data_ref != LUA_NOREF) {
    luaL_unref(L, LUA_REGISTRYINDEX, write_data_ref);
  }

  luaio_timer_free(handle);
  luaio_req->timer = NULL;
}

static void luaio_tcp_socket_after_write_async(uv_write_t *req, int status) {
  luaio_tcp_write_req_t *luaio_req = container_of(req, luaio_tcp_write_req_t, req);
  lua_State* L = luaio_get_main_thread();
 
  uv_timer_t *timer = luaio_req->timer;
  if (timer != NULL) {
    uv_timer_stop(timer);
    luaio_timer_free(timer);
  }

  int write_data_ref = luaio_req->write_data_ref;
  if (write_data_ref != LUA_NOREF) {
    luaL_unref(L, LUA_REGISTRYINDEX, write_data_ref);
  }

  luaio_pfree(luaio_req);
}

/*local bytes, err = socket:write_async(data)*/
static int luaio_tcp_socket_write_async(lua_State *L) {
  luaio_tcp_check_socket(L, write(data));
  /*common.h*/
  luaio_check_data(L, 2, socket:write(data));

  size_t written = 0;
  size_t vcount = count;
  uv_stream_t *stream_handle = (uv_stream_t*)(&socket->handle);
  int err = luaio_tcp_socket_try_write(stream_handle, 
                                       &bufs, 
                                       &vcount, 
                                       &written);
  if (err) {
    if (tmp != NULL) {
      luaio_stack_buffer_free(&stack_buf);
    }

    lua_pushinteger(L, 0);
    lua_pushinteger(L, err);
    return 2;
  }

  /*uv_try_write send all data*/
  if (vcount == 0) {
    if (tmp != NULL) {
      luaio_stack_buffer_free(&stack_buf);
    }

    lua_pushinteger(L, written);
    lua_pushinteger(L, 0);
    return 2;
  }

  uint64_t timeout = socket->timeout;
  uv_timer_t *timer = NULL;
  if (timeout != 0) {
    timer = luaio_timer_alloc();
    if (timer == NULL) {
      if (tmp != NULL) {
        luaio_stack_buffer_free(&stack_buf);
      }

      lua_pushinteger(L, written);
      lua_pushinteger(L, UV_ENOMEM);
      return 2;
    }

    uv_timer_start(timer, 
                   luaio_tcp_socket_write_async_timeout, 
                   timeout, 
                   0);
  }

  luaio_tcp_write_req_t *luaio_req = luaio_palloc(sizeof(luaio_tcp_write_req_t));
  if (luaio_req == NULL) {
    if (tmp != NULL) {
      luaio_stack_buffer_free(&stack_buf);
    }

    if (timer != NULL) {
      uv_timer_stop(timer);
      luaio_timer_free(timer);
    }

    lua_pushinteger(L, written);
    lua_pushinteger(L, UV_ENOMEM);
    return 2;
  }

  err = uv_write2(&luaio_req->req, 
                  stream_handle, 
                  bufs, 
                  vcount, 
                  NULL, 
                  luaio_tcp_socket_after_write_async);
  if (err) {
    if (tmp != NULL) {
      luaio_stack_buffer_free(&stack_buf);
    }

    if (timer != NULL) {
      uv_timer_stop(timer);
      luaio_timer_free(timer);
    }

    luaio_pfree(luaio_req);
    lua_pushinteger(L, written);
    lua_pushinteger(L, err);
    return 2;
  }

  lua_pushvalue(L, 2);
  luaio_req->write_data_ref = luaL_ref(L, LUA_REGISTRYINDEX);
  luaio_req->current_thread = NULL;
  luaio_req->timer = timer;

  if (timer != NULL) {
    timer->data = luaio_req;
  }

  if (tmp != NULL) {
    luaio_stack_buffer_free(&stack_buf);
  }

  lua_pushinteger(L, bytes);
  lua_pushinteger(L, 0);
  return 2;
}


/*local addr, err = socket:local_address()*/
static int luaio_tcp_socket_local_address(lua_State *L) {
  luaio_tcp_check_socket(L, localAddress());

  struct sockaddr_storage address;
  int len = sizeof(address);
  int ret = uv_tcp_getsockname(&socket->handle, 
                               (struct sockaddr*)&address, &len);
  if (ret == 0) {
    ret = luaio_parse_socket_address(L, &address);
  } else {
    lua_pushnil(L);
  }

  lua_pushinteger(L, ret);
  return 2;
}

/*local addr, err = socket:remote_address()*/
static int luaio_tcp_socket_remote_address(lua_State *L) {
  luaio_tcp_check_socket(L, remoteAddress());

  struct sockaddr_storage address;
  int len = sizeof(address);
  int ret = uv_tcp_getpeername(&socket->handle, 
                               (struct sockaddr*)&address, &len);
  if (ret == 0) {
    ret = luaio_parse_socket_address(L, &address);
  } else {
    lua_pushnil(L);
  }

  lua_pushinteger(L, ret);
  return 2;
}

/*socket:set_timeout(timeout)*/
static int luaio_tcp_socket_set_timeout(lua_State *L) {
  luaio_tcp_check_socket(L, setTimeout(timeout));

  lua_Integer timeout = luaL_checkinteger(L, 2);
  if (timeout < 0) {
    return luaL_argerror(L, 1, "socket:setTimeout(timeout) error: timeout must be >= 0\n");
  }
  socket->timeout = timeout;

  return 0;
}

/*local err = socket:set_nodelay(enable)*/
static int luaio_tcp_socket_set_nodelay(lua_State *L) {
  luaio_tcp_check_socket(L, setNodelay(enable));

  int enable = lua_toboolean(L, 2);
  int err = uv_tcp_nodelay(&socket->handle, enable);

  lua_pushinteger(L, err);
  return 1;
}

/*local err = socket:set_keepalive(enable, delay)*/
static int luaio_tcp_socket_set_keepalive(lua_State *L) {
  luaio_tcp_check_socket(L, setKeepalive(enable, delay));

  int delay = 0;
  int enable = lua_toboolean(L, 2);
  if (enable) {
    delay = luaL_checkinteger(L, 3);
  }
  int err = uv_tcp_keepalive(&socket->handle, enable, delay);

  lua_pushinteger(L, err);
  return 1;
}

static void luaio_tcp_socket_after_shutdown(uv_shutdown_t *req, int status) {
  lua_State *L = req->data;
  luaio_pfree(req);
  lua_pushinteger(L, status);
  luaio_resume(L, 1);
}

/*local err = socket:shutdown()*/
static int luaio_tcp_socket_shutdown(lua_State *L) {
  luaio_tcp_check_socket(L, shutdown());

  uv_shutdown_t *req = luaio_palloc(sizeof(uv_shutdown_t));
  if (req == NULL) {
    lua_pushinteger(L, UV_ENOMEM);
    return 1;
  }

  req->data = L;
  int err = uv_shutdown(req, 
                        (uv_stream_t*)(&socket->handle), 
                        luaio_tcp_socket_after_shutdown);
  if (err) {
    luaio_pfree(req);
    lua_pushinteger(L, err);
    return 1;
  }

  return lua_yield(L, 0);
}

static void luaio_tcp_socket_onclose(uv_handle_t *handle) {
  luaio_tcp_socket_t *socket = container_of(handle, luaio_tcp_socket_t, handle);
  lua_State *L = socket->current_thread;

  /*free read timer*/
  uv_timer_t *timer = socket->timer;
  if (timer != NULL) {
    uv_timer_stop(timer);
    luaio_timer_free(timer);
  }

  int onconnect_ref = socket->onconnect_ref;
  if (onconnect_ref != LUA_NOREF) {
    luaL_unref(L, LUA_REGISTRYINDEX, onconnect_ref);
    socket->onconnect_ref = LUA_NOREF;
  }

  int thread_ref = socket->thread_ref;
  if (thread_ref != LUA_NOREF) {
    luaL_unref(L, LUA_REGISTRYINDEX, thread_ref);
    socket->thread = NULL;
    socket->thread_ref = LUA_NOREF;
  }

  luaio_resume(L, 0);
}

/*socket:close()*/
static int luaio_tcp_socket_close(lua_State *L) {
  luaio_tcp_check_socket(L, close());

  uv_handle_t *handle = (uv_handle_t*)(&socket->handle);
  if (uv_is_closing(handle)) {
    luaL_error(L, "socket:close() error: socket is already closing");
  }

  uv_close(handle, luaio_tcp_socket_onclose);

  socket->current_thread = L;
  return lua_yield(L, 0);
}

/*tcp.is_ip(string)*/
static int luaio_tcp_is_ip(lua_State *L) {
  const char *ip = luaL_checkstring(L, 1);
  char addr[sizeof(struct in6_addr)];
  
  int rc = 0;
  if (uv_inet_pton(AF_INET, ip, &addr) == 0) {
    rc = 4;
  } else if (uv_inet_pton(AF_INET6, ip, &addr) == 0) {
    rc = 6;
  }

  lua_pushinteger(L, rc);
  return 1;
}

int luaopen_tcp(lua_State *L) {
  /*tcp socket metatable*/
  luaL_Reg tcp_socket_mtlib[] = {
    { "bind", luaio_tcp_socket_bind },
    { "listen", luaio_tcp_socket_listen },
    { "connect", luaio_tcp_socket_connect },
    { "fd", luaio_tcp_socket_fd },
    { "set_read_buffer", luaio_tcp_socket_set_read_buffer },
    { "read", luaio_tcp_socket_read },
    /*yield from current thread, resume to current thread withe success, error, timeout message*/
    { "write", luaio_tcp_socket_write },
    /*not yeild from current thread, ignore success, error, timeout message*/
    { "write_async", luaio_tcp_socket_write_async },
    { "local_address", luaio_tcp_socket_local_address },
    { "remote_address", luaio_tcp_socket_remote_address },
    { "set_timeout", luaio_tcp_socket_set_timeout },
    { "set_nodelay", luaio_tcp_socket_set_nodelay },
    { "set_keepalive", luaio_tcp_socket_set_keepalive },
    { "shutdown", luaio_tcp_socket_shutdown },
    { "close", luaio_tcp_socket_close },
    { NULL, NULL }
  };

  lua_pushlightuserdata(L, &luaio_tcp_socket_metatable_key);
  luaL_newlib(L, tcp_socket_mtlib);
  lua_pushvalue(L, -1);
  lua_setfield(L, -2, "__index");
  lua_rawset(L, LUA_REGISTRYINDEX);

  luaL_Reg lib[] = {
    { "new", luaio_tcp_socket_new },
    { "is_ip", luaio_tcp_is_ip },
    { "__newindex", luaio_cannot_change },
    { NULL, NULL }
  };

  lua_createtable(L, 0, 0);

  luaL_newlib(L, lib);
  lua_pushvalue(L, -1);
  lua_setfield(L, -2, "__index");
  lua_pushliteral(L, "metatable is protected.");
  lua_setfield(L, -2, "__metatable");

  lua_setmetatable(L, -2);

  return 1;
}
