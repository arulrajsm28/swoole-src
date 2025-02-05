/*
  +----------------------------------------------------------------------+
  | Swoole                                                               |
  +----------------------------------------------------------------------+
  | This source file is subject to version 2.0 of the Apache license,    |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.apache.org/licenses/LICENSE-2.0.html                      |
  | If you did not receive a copy of the Apache2.0 license and are unable|
  | to obtain it through the world-wide-web, please send a note to       |
  | license@swoole.com so we can mail you a copy immediately.            |
  +----------------------------------------------------------------------+
  | Author: Tianfeng Han  <mikan.tenny@gmail.com>                        |
  +----------------------------------------------------------------------+
*/

#include "php_swoole_cxx.h"
#include "redis.h"

#include <unordered_map>
#include <string>

BEGIN_EXTERN_C()
#include "ext/standard/php_string.h"
END_EXTERN_C()

using namespace swoole;
using namespace std;

static zend_class_entry *swoole_redis_server_ce;
static zend_object_handlers swoole_redis_server_handlers;

static swString *format_buffer;
static unordered_map<string, zend_fcall_info_cache *> redis_handlers;

static PHP_METHOD(swoole_redis_server, start);
static PHP_METHOD(swoole_redis_server, setHandler);
static PHP_METHOD(swoole_redis_server, getHandler);
static PHP_METHOD(swoole_redis_server, format);

ZEND_BEGIN_ARG_INFO_EX(arginfo_swoole_redis_server_start, 0, 0, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_swoole_redis_server_setHandler, 0, 0, 2)
    ZEND_ARG_INFO(0, command)
    ZEND_ARG_CALLABLE_INFO(0, callback, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_swoole_redis_server_getHandler, 0, 0, 1)
    ZEND_ARG_INFO(0, command)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_swoole_redis_server_format, 0, 0, 1)
    ZEND_ARG_INFO(0, type)
    ZEND_ARG_INFO(0, value)
ZEND_END_ARG_INFO()

const zend_function_entry swoole_redis_server_methods[] =
{
    PHP_ME(swoole_redis_server, start, arginfo_swoole_redis_server_start, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_redis_server, setHandler, arginfo_swoole_redis_server_setHandler, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_redis_server, getHandler, arginfo_swoole_redis_server_getHandler, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_redis_server, format, arginfo_swoole_redis_server_format, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_FE_END
};

void swoole_redis_server_init(int module_number)
{
    SW_INIT_CLASS_ENTRY_EX(swoole_redis_server, "Swoole\\Redis\\Server", "swoole_redis_server", NULL, swoole_redis_server_methods, swoole_server);
    SW_SET_CLASS_SERIALIZABLE(swoole_redis_server, zend_class_serialize_deny, zend_class_unserialize_deny);
    SW_SET_CLASS_CLONEABLE(swoole_redis_server, zend_class_clone_deny);
    SW_SET_CLASS_UNSET_PROPERTY_HANDLER(swoole_redis_server, sw_zend_class_unset_property_deny);
    SW_SET_CLASS_CREATE_WITH_ITS_OWN_HANDLERS(swoole_redis_server);

    zend_declare_class_constant_long(swoole_redis_server_ce, ZEND_STRL("NIL"), SW_REDIS_REPLY_NIL);
    zend_declare_class_constant_long(swoole_redis_server_ce, ZEND_STRL("ERROR"), SW_REDIS_REPLY_ERROR);
    zend_declare_class_constant_long(swoole_redis_server_ce, ZEND_STRL("STATUS"), SW_REDIS_REPLY_STATUS);
    zend_declare_class_constant_long(swoole_redis_server_ce, ZEND_STRL("INT"), SW_REDIS_REPLY_INT);
    zend_declare_class_constant_long(swoole_redis_server_ce, ZEND_STRL("STRING"), SW_REDIS_REPLY_STRING);
    zend_declare_class_constant_long(swoole_redis_server_ce, ZEND_STRL("SET"), SW_REDIS_REPLY_SET);
    zend_declare_class_constant_long(swoole_redis_server_ce, ZEND_STRL("MAP"), SW_REDIS_REPLY_MAP);
}

void swoole_redis_server_shutdown()
{
    for (auto i = redis_handlers.begin(); i != redis_handlers.end(); i++)
    {
        zend_fcall_info_cache *fci_cache = i->second;
        sw_fci_cache_discard(fci_cache);
        efree(fci_cache);
    }
}

static int redis_onReceive(swServer *serv, swEventData *req)
{
    int fd = req->info.fd;
    swConnection *conn = swWorker_get_connection(serv, fd);
    if (!conn)
    {
        swWarn("connection[%d] is closed", fd);
        return SW_ERR;
    }

    swListenPort *port = swServer_get_port(serv, conn->fd);
    //other server port
    if (!port->open_redis_protocol)
    {
        return php_swoole_onReceive(serv, req);
    }

    zval zdata;
    php_swoole_get_recv_data(&zdata, req, NULL, 0);
    char *p = Z_STRVAL(zdata);
    char *pe = p + Z_STRLEN(zdata);
    int ret;
    int length = 0;

    zval zparams;
    array_init(&zparams);

    int state = SW_REDIS_RECEIVE_TOTAL_LINE;
    int add_param = 0;
    char *command = NULL;
    int command_len = 0;

    do
    {
        switch (state)
        {
        case SW_REDIS_RECEIVE_TOTAL_LINE:
            if (*p == '*' && (p = swRedis_get_number(p, &ret)))
            {
                state = SW_REDIS_RECEIVE_LENGTH;
                break;
            }
            /* no break */

        case SW_REDIS_RECEIVE_LENGTH:
            if (*p == '$' && (p = swRedis_get_number(p, &ret)))
            {
                if (ret == -1)
                {
                    add_next_index_null(&zparams);
                    break;
                }
                length = ret;
                state = SW_REDIS_RECEIVE_STRING;
                break;
            }
            //integer
            else if (*p == ':' && (p = swRedis_get_number(p, &ret)))
            {
                add_next_index_long(&zparams, ret);
                break;
            }
            /* no break */

        case SW_REDIS_RECEIVE_STRING:
            if (add_param == 0)
            {
                command = p;
                command_len = length;
                add_param = 1;
            }
            else
            {
                add_next_index_stringl(&zparams, p, length);
            }
            p += length + SW_CRLF_LEN;
            state = SW_REDIS_RECEIVE_LENGTH;
            break;

        default:
            break;
        }
    } while(p < pe);

    if (command_len >= SW_REDIS_MAX_COMMAND_SIZE)
    {
        swoole_php_error(E_WARNING, "command is too long");
        serv->close(serv, fd, 0);
        return SW_OK;
    }

    char _command[SW_REDIS_MAX_COMMAND_SIZE];
    command[command_len] = 0;
    size_t _command_len = sw_snprintf(_command, sizeof(_command), "_handler_%.*s", command_len, command);
    php_strtolower(_command, _command_len);

    zval args[2];
    ZVAL_LONG(&args[0], fd);
    args[1] = zparams;

    auto i = redis_handlers.find(string(_command, _command_len));
    if (i == redis_handlers.end())
    {
        char err_msg[256];
        length = sw_snprintf(err_msg, sizeof(err_msg), "-ERR unknown command '%.*s'\r\n", command_len, command);
        serv->send(serv, fd, err_msg, length);
        return SW_OK;
    }

    zend_fcall_info_cache *fci_cache = i->second;

    if (SwooleG.enable_coroutine)
    {
        if (PHPCoroutine::create(fci_cache, 2, args) < 0)
        {
            swoole_php_error(E_WARNING, "create redis server onReceive coroutine error");
        }
    }
    else
    {
        zval _retval, *retval = &_retval;
        if (sw_call_user_function_fast_ex(NULL, fci_cache, retval, 2, args) == FAILURE)
        {
            swoole_php_error(E_WARNING, "redis server command '%.*s' handler error", command_len, command);
        }
        if (Z_TYPE_P(retval) == IS_STRING)
        {
            serv->send(serv, fd, Z_STRVAL_P(retval), Z_STRLEN_P(retval));
        }
        zval_ptr_dtor(retval);
    }

    zval_ptr_dtor(&zdata);
    zval_ptr_dtor(&zparams);

    return SW_OK;
}

static PHP_METHOD(swoole_redis_server, start)
{
    int ret;

    swServer *serv = (swServer *) swoole_get_object(getThis());
    if (serv->gs->start > 0)
    {
        swoole_php_error(E_WARNING, "Server is running. Unable to execute swoole_server::start");
        RETURN_FALSE;
    }

    php_swoole_register_callback(serv);

    serv->onReceive = redis_onReceive;

    format_buffer = swString_new(SW_BUFFER_SIZE_STD);
    if (!format_buffer)
    {
        swoole_php_fatal_error(E_ERROR, "[1] swString_new(%d) failed", SW_BUFFER_SIZE_STD);
        RETURN_FALSE;
    }

    zval *zsetting = sw_zend_read_and_convert_property_array(swoole_server_ce, getThis(), ZEND_STRL("setting"), 0);
#ifdef HT_ALLOW_COW_VIOLATION
    HT_ALLOW_COW_VIOLATION(Z_ARRVAL_P(zsetting));
#endif

    add_assoc_bool(zsetting, "open_http_protocol", 0);
    add_assoc_bool(zsetting, "open_mqtt_protocol", 0);
    add_assoc_bool(zsetting, "open_eof_check", 0);
    add_assoc_bool(zsetting, "open_length_check", 0);
    add_assoc_bool(zsetting, "open_redis_protocol", 0);

    serv->listen_list->open_http_protocol = 0;
    serv->listen_list->open_mqtt_protocol = 0;
    serv->listen_list->open_eof_check = 0;
    serv->listen_list->open_length_check = 0;
    serv->listen_list->open_redis_protocol = 1;

    php_swoole_server_before_start(serv, getThis());

    ret = swServer_start(serv);
    if (ret < 0)
    {
        swoole_php_fatal_error(E_ERROR, "server failed to start. Error: %s", sw_error);
        RETURN_LONG(ret);
    }
    RETURN_TRUE;
}

static PHP_METHOD(swoole_redis_server, setHandler)
{
    char *command;
    size_t command_len;
    zval *zcallback;

    if (zend_parse_parameters(ZEND_NUM_ARGS(), "sz", &command, &command_len, &zcallback) == FAILURE)
    {
        RETURN_FALSE;
    }

    if (command_len == 0 || command_len >= SW_REDIS_MAX_COMMAND_SIZE)
    {
        swoole_php_fatal_error(E_ERROR, "invalid command");
        RETURN_FALSE;
    }

    char *func_name;
    zend_fcall_info_cache *func_cache = (zend_fcall_info_cache *) emalloc(sizeof(zend_fcall_info_cache));
    if (!sw_zend_is_callable_ex(zcallback, NULL, 0, &func_name, NULL, func_cache, NULL))
    {
        swoole_php_fatal_error(E_ERROR, "function '%s' is not callable", func_name);
        return;
    }
    efree(func_name);

    char _command[SW_REDIS_MAX_COMMAND_SIZE];
    size_t _command_len = sw_snprintf(_command, sizeof(_command), "_handler_%s", command);
    php_strtolower(_command, _command_len);

    zend_update_property(swoole_redis_server_ce, getThis(), _command, _command_len, zcallback);

    string key(_command, _command_len);
    auto i = redis_handlers.find(key);
    if (i != redis_handlers.end())
    {
        sw_fci_cache_discard(i->second);
        efree(i->second);
    }

    sw_fci_cache_persist(func_cache);
    redis_handlers[key] = func_cache;

    RETURN_TRUE;
}

static PHP_METHOD(swoole_redis_server, getHandler)
{
    char *command;
    size_t command_len;

    if (zend_parse_parameters(ZEND_NUM_ARGS(), "s", &command, &command_len) == FAILURE)
    {
        RETURN_FALSE;
    }

    char _command[SW_REDIS_MAX_COMMAND_SIZE];
    size_t _command_len = sw_snprintf(_command, sizeof(_command), "_handler_%s", command);
    php_strtolower(_command, _command_len);

    zval rv;
    zval *handler = zend_read_property(swoole_redis_server_ce, getThis(), _command, _command_len, 1, &rv);
    RETURN_ZVAL(handler, 1, 0);
}

static PHP_METHOD(swoole_redis_server, format)
{
    zend_long type;
    zval *value = NULL;

    if (zend_parse_parameters(ZEND_NUM_ARGS(), "l|z", &type, &value) == FAILURE)
    {
        RETURN_FALSE;
    }

    char message[256];
    int length;
    zval *item;

    if (type == SW_REDIS_REPLY_NIL)
    {
        RETURN_STRINGL(SW_REDIS_RETURN_NIL, sizeof(SW_REDIS_RETURN_NIL)-1);
    }
    else if (type == SW_REDIS_REPLY_STATUS)
    {
        if (value)
        {
            convert_to_string(value);
            length = sw_snprintf(message, sizeof(message), "+%.*s\r\n", (int)Z_STRLEN_P(value), Z_STRVAL_P(value));
        }
        else
        {
            length = sw_snprintf(message, sizeof(message), "+%s\r\n", "OK");
        }
        RETURN_STRINGL(message, length);
    }
    else if (type == SW_REDIS_REPLY_ERROR)
    {
        if (value)
        {
            convert_to_string(value);
            length = sw_snprintf(message, sizeof(message), "-%.*s\r\n", (int)Z_STRLEN_P(value), Z_STRVAL_P(value));
        }
        else
        {
            length = sw_snprintf(message, sizeof(message), "-%s\r\n", "ERR");
        }
        RETURN_STRINGL(message, length);
    }
    else if (type == SW_REDIS_REPLY_INT)
    {
        if (!value)
        {
            goto no_value;
        }

        length = sw_snprintf(message, sizeof(message), ":" ZEND_LONG_FMT "\r\n", zval_get_long(value));
        RETURN_STRINGL(message, length);
    }
    else if (type == SW_REDIS_REPLY_STRING)
    {
        if (!value)
        {
            no_value:
            swoole_php_fatal_error(E_WARNING, "require more parameters");
            RETURN_FALSE;
        }
        convert_to_string(value);
        if (Z_STRLEN_P(value) > SW_REDIS_MAX_STRING_SIZE || Z_STRLEN_P(value) < 1)
        {
            swoole_php_fatal_error(E_WARNING, "invalid string size");
            RETURN_FALSE;
        }
        swString_clear(format_buffer);
        length = sw_snprintf(message, sizeof(message), "$%zu\r\n", Z_STRLEN_P(value));
        swString_append_ptr(format_buffer, message, length);
        swString_append_ptr(format_buffer, Z_STRVAL_P(value), Z_STRLEN_P(value));
        swString_append_ptr(format_buffer, SW_CRLF, SW_CRLF_LEN);
        RETURN_STRINGL(format_buffer->str, format_buffer->length);
    }
    else if (type == SW_REDIS_REPLY_SET)
    {
        if (!value)
        {
            goto no_value;
        }
        if (Z_TYPE_P(value) != IS_ARRAY)
        {
            swoole_php_fatal_error(E_WARNING, "the second parameter should be an array");
        }
        swString_clear(format_buffer);
        length = sw_snprintf(message, sizeof(message), "*%d\r\n", zend_hash_num_elements(Z_ARRVAL_P(value)));
        swString_append_ptr(format_buffer, message, length);

        SW_HASHTABLE_FOREACH_START(Z_ARRVAL_P(value), item)
            zval _copy;
            if (Z_TYPE_P(item) != IS_STRING)
            {
                _copy = *item;
                zval_copy_ctor(&_copy);
                item = &_copy;
            }
            convert_to_string(item);
            length = sw_snprintf(message, sizeof(message), "$%zu\r\n", Z_STRLEN_P(item));
            swString_append_ptr(format_buffer, message, length);
            swString_append_ptr(format_buffer, Z_STRVAL_P(item), Z_STRLEN_P(item));
            swString_append_ptr(format_buffer, SW_CRLF, SW_CRLF_LEN);
            if (item == &_copy)
            {
                zval_dtor(item);
            }
        SW_HASHTABLE_FOREACH_END();

        RETURN_STRINGL(format_buffer->str, format_buffer->length);
    }
    else if (type == SW_REDIS_REPLY_MAP)
    {
        if (!value)
        {
            goto no_value;
        }
        if (Z_TYPE_P(value) != IS_ARRAY)
        {
            swoole_php_fatal_error(E_WARNING, "the second parameter should be an array");
        }
        swString_clear(format_buffer);
        length = sw_snprintf(message, sizeof(message), "*%d\r\n", 2 * zend_hash_num_elements(Z_ARRVAL_P(value)));
        swString_append_ptr(format_buffer, message, length);

        char *key;
        uint32_t keylen;
        int keytype;

        SW_HASHTABLE_FOREACH_START2(Z_ARRVAL_P(value), key, keylen, keytype, item)
            if (key == NULL || keylen == 0)
            {
                continue;
            }
            zval _copy;
            if (Z_TYPE_P(item) != IS_STRING)
            {
                _copy = *item;
                zval_copy_ctor(&_copy);
                item = &_copy;
            }
            convert_to_string(item);
            length = sw_snprintf(message, sizeof(message), "$%d\r\n%s\r\n$%zu\r\n", keylen, key, Z_STRLEN_P(item));
            swString_append_ptr(format_buffer, message, length);
            swString_append_ptr(format_buffer, Z_STRVAL_P(item), Z_STRLEN_P(item));
            swString_append_ptr(format_buffer, SW_CRLF, SW_CRLF_LEN);

            if (item == &_copy)
            {
                zval_dtor(item);
            }
            (void) keytype;
        SW_HASHTABLE_FOREACH_END();

        RETURN_STRINGL(format_buffer->str, format_buffer->length);
    }
    else
    {
        swoole_php_error(E_WARNING, "Unknown type[" ZEND_LONG_FMT "]", type);
        RETURN_FALSE;
    }
}
