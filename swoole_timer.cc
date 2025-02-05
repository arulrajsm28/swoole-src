/*
 +----------------------------------------------------------------------+
 | Swoole                                                               |
 +----------------------------------------------------------------------+
 | Copyright (c) 2012-2015 The Swoole Group                             |
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

#include "php_swoole.h"
#include "swoole_coroutine.h"

#include "ext/spl/spl_array.h"

using namespace swoole;

zend_class_entry *swoole_timer_ce;
static zend_object_handlers swoole_timer_handlers;

static zend_class_entry *swoole_timer_iterator_ce;

static PHP_FUNCTION(swoole_timer_after);
static PHP_FUNCTION(swoole_timer_tick);
static PHP_FUNCTION(swoole_timer_exists);
static PHP_FUNCTION(swoole_timer_info);
static PHP_FUNCTION(swoole_timer_stats);
static PHP_FUNCTION(swoole_timer_list);
static PHP_FUNCTION(swoole_timer_clear);
static PHP_FUNCTION(swoole_timer_clear_all);

ZEND_BEGIN_ARG_INFO_EX(arginfo_swoole_void, 0, 0, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_swoole_timer_after, 0, 0, 2)
    ZEND_ARG_INFO(0, ms)
    ZEND_ARG_CALLABLE_INFO(0, callback, 0)
    ZEND_ARG_VARIADIC_INFO(0, params)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_swoole_timer_tick, 0, 0, 2)
    ZEND_ARG_INFO(0, ms)
    ZEND_ARG_CALLABLE_INFO(0, callback, 0)
    ZEND_ARG_VARIADIC_INFO(0, params)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_swoole_timer_exists, 0, 0, 1)
    ZEND_ARG_INFO(0, timer_id)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_swoole_timer_info, 0, 0, 1)
    ZEND_ARG_INFO(0, timer_id)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_swoole_timer_clear, 0, 0, 1)
    ZEND_ARG_INFO(0, timer_id)
ZEND_END_ARG_INFO()

static const zend_function_entry swoole_timer_methods[] =
{
    ZEND_FENTRY(tick, ZEND_FN(swoole_timer_tick), arginfo_swoole_timer_tick, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    ZEND_FENTRY(after, ZEND_FN(swoole_timer_after), arginfo_swoole_timer_after, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    ZEND_FENTRY(exists, ZEND_FN(swoole_timer_exists), arginfo_swoole_timer_exists, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    ZEND_FENTRY(info, ZEND_FN(swoole_timer_info), arginfo_swoole_timer_info, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    ZEND_FENTRY(stats, ZEND_FN(swoole_timer_stats), arginfo_swoole_void, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    ZEND_FENTRY(list, ZEND_FN(swoole_timer_list), arginfo_swoole_void, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    ZEND_FENTRY(clear, ZEND_FN(swoole_timer_clear), arginfo_swoole_timer_clear, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    ZEND_FENTRY(clearAll, ZEND_FN(swoole_timer_clear_all), arginfo_swoole_void, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_FE_END
};

void swoole_timer_init(int module_number)
{
    SW_INIT_CLASS_ENTRY(swoole_timer, "Swoole\\Timer", "swoole_timer", NULL, swoole_timer_methods);
    SW_SET_CLASS_CREATE(swoole_timer, sw_zend_create_object_deny);

    SW_INIT_CLASS_ENTRY_BASE(swoole_timer_iterator, "Swoole\\Timer\\Iterator", "swoole_timer_iterator", NULL, NULL, spl_ce_ArrayIterator);

    SW_FUNCTION_ALIAS(&swoole_timer_ce->function_table, "after", CG(function_table), "swoole_timer_after");
    SW_FUNCTION_ALIAS(&swoole_timer_ce->function_table, "tick", CG(function_table), "swoole_timer_tick");
    SW_FUNCTION_ALIAS(&swoole_timer_ce->function_table, "exists", CG(function_table), "swoole_timer_exists");
    SW_FUNCTION_ALIAS(&swoole_timer_ce->function_table, "info", CG(function_table), "swoole_timer_info");
    SW_FUNCTION_ALIAS(&swoole_timer_ce->function_table, "stats", CG(function_table), "swoole_timer_stats");
    SW_FUNCTION_ALIAS(&swoole_timer_ce->function_table, "list", CG(function_table), "swoole_timer_list");
    SW_FUNCTION_ALIAS(&swoole_timer_ce->function_table, "clear", CG(function_table), "swoole_timer_clear");
    SW_FUNCTION_ALIAS(&swoole_timer_ce->function_table, "clearAll", CG(function_table), "swoole_timer_clear_all");
}

static void php_swoole_timer_dtor(swTimer_node *tnode)
{
    php_swoole_fci *fci = (php_swoole_fci *) tnode->data;
    sw_fci_params_discard(&fci->fci);
    sw_fci_cache_discard(&fci->fci_cache);
    efree(fci);
}

enum swBool_type php_swoole_timer_clear(swTimer_node *tnode)
{
    return swTimer_del_ex(&SwooleG.timer, tnode, php_swoole_timer_dtor);
}

enum swBool_type php_swoole_timer_clear_all()
{
    if (UNEXPECTED(!SwooleG.timer.map))
    {
        return SW_FALSE;
    }
    swHashMap_rewind(SwooleG.timer.map);
    while (1)
    {
        uint64_t timer_id;
        swTimer_node *tnode = (swTimer_node *) swHashMap_each_int(SwooleG.timer.map, &timer_id);
        if (UNEXPECTED(!tnode))
        {
            break;
        }
        if (tnode->type == SW_TIMER_TYPE_PHP)
        {
            swTimer_del_ex(&SwooleG.timer, tnode, php_swoole_timer_dtor);
        }
    }
    return SW_TRUE;
}

static void php_swoole_onTimeout(swTimer *timer, swTimer_node *tnode)
{
    php_swoole_fci *fci = (php_swoole_fci *) tnode->data;

    if (SwooleG.enable_coroutine)
    {
        if (PHPCoroutine::create(&fci->fci_cache, fci->fci.param_count, fci->fci.params) < 0)
        {
            swoole_php_fatal_error(E_WARNING, "create onTimer coroutine error");
        }
    }
    else
    {
        zval retval;
        if (sw_call_user_function_fast_ex(NULL, &fci->fci_cache, &retval, fci->fci.param_count, fci->fci.params) == FAILURE)
        {
            swoole_php_fatal_error(E_WARNING, "onTimeout handler error");
        }
        zval_ptr_dtor(&retval);
    }

    if (!tnode->interval || tnode->removed)
    {
        php_swoole_timer_dtor(tnode);
    }
}

static void php_swoole_timer_add(INTERNAL_FUNCTION_PARAMETERS, bool persistent)
{
    zend_long ms;
    php_swoole_fci *fci = (php_swoole_fci *) ecalloc(1, sizeof(php_swoole_fci));
    swTimer_node *tnode;

    ZEND_PARSE_PARAMETERS_START(2, -1)
        Z_PARAM_LONG(ms)
        Z_PARAM_FUNC(fci->fci, fci->fci_cache)
        Z_PARAM_VARIADIC('*', fci->fci.params, fci->fci.param_count)
    ZEND_PARSE_PARAMETERS_END_EX(goto _failed);

    if (UNEXPECTED(ms <= 0))
    {
        swoole_php_fatal_error(E_WARNING, "Timer must be greater than 0");
        _failed:
        efree(fci);
        RETURN_FALSE;
    }

    // no server || user worker || task process with async mode
    if (!SwooleG.serv || swIsUserWorker() || (swIsTaskWorker() && SwooleG.serv->task_enable_coroutine))
    {
        php_swoole_check_reactor();
    }

    tnode = swTimer_add(&SwooleG.timer, ms, persistent, fci, php_swoole_onTimeout);
    if (UNEXPECTED(!tnode))
    {
        swoole_php_fatal_error(E_WARNING, "add timer failed");
        goto _failed;
    }
    tnode->type = SW_TIMER_TYPE_PHP;
    if (persistent)
    {
        if (fci->fci.param_count > 0)
        {
            uint32_t i;
            zval *params = (zval *) ecalloc(fci->fci.param_count + 1, sizeof(zval));
            for (i = 0; i < fci->fci.param_count; i++)
            {
                ZVAL_COPY(&params[i + 1], &fci->fci.params[i]);
            }
            fci->fci.params = params;
        }
        else
        {
            fci->fci.params = (zval *) emalloc(sizeof(zval));
        }
        fci->fci.param_count += 1;
        ZVAL_LONG(fci->fci.params, tnode->id);
    }
    else
    {
        sw_fci_params_persist(&fci->fci);
    }
    sw_fci_cache_persist(&fci->fci_cache);
    RETURN_LONG(tnode->id);
}

static PHP_FUNCTION(swoole_timer_after)
{
    php_swoole_timer_add(INTERNAL_FUNCTION_PARAM_PASSTHRU, false);
}

static PHP_FUNCTION(swoole_timer_tick)
{
    php_swoole_timer_add(INTERNAL_FUNCTION_PARAM_PASSTHRU, true);
}

static PHP_FUNCTION(swoole_timer_exists)
{
    if (UNEXPECTED(!SwooleG.timer.initialized))
    {
        RETURN_FALSE;
    }
    else
    {
        zend_long id;
        swTimer_node *tnode;

        ZEND_PARSE_PARAMETERS_START(1, 1)
            Z_PARAM_LONG(id)
        ZEND_PARSE_PARAMETERS_END_EX(RETURN_FALSE);

        tnode = swTimer_get(&SwooleG.timer, id);
        RETURN_BOOL(tnode && !tnode->removed);
    }
}

static PHP_FUNCTION(swoole_timer_info)
{
    if (UNEXPECTED(!SwooleG.timer.initialized))
    {
        RETURN_FALSE;
    }
    else
    {
        zend_long id;
        swTimer_node *tnode;

        ZEND_PARSE_PARAMETERS_START(1, 1)
            Z_PARAM_LONG(id)
        ZEND_PARSE_PARAMETERS_END_EX(RETURN_FALSE);

        tnode = swTimer_get(&SwooleG.timer, id);
        if (UNEXPECTED(!tnode))
        {
            RETURN_NULL();
        }
        array_init(return_value);
        add_assoc_long(return_value, "exec_msec", tnode->exec_msec);
        add_assoc_long(return_value, "interval", tnode->interval);
        add_assoc_long(return_value, "round", tnode->round);
        add_assoc_bool(return_value, "removed", tnode->removed);
    }
}

static PHP_FUNCTION(swoole_timer_stats)
{
    array_init(return_value);
    add_assoc_bool(return_value, "initialized", SwooleG.timer.initialized);
    add_assoc_long(return_value, "num", SwooleG.timer.num);
    add_assoc_long(return_value, "round", SwooleG.timer.round);
}

static PHP_FUNCTION(swoole_timer_list)
{
    zval zlist;
    array_init(&zlist);
    if (EXPECTED(SwooleG.timer.initialized))
    {
        swHashMap_rewind(SwooleG.timer.map);
        while (1)
        {
            uint64_t timer_id;
            swTimer_node *tnode = (swTimer_node *) swHashMap_each_int(SwooleG.timer.map, &timer_id);
            if (UNEXPECTED(!tnode))
            {
                break;
            }
            if (tnode->type == SW_TIMER_TYPE_PHP)
            {
                add_next_index_long(&zlist, timer_id);
            }
        }
    }
    object_init_ex(return_value, swoole_timer_iterator_ce);
    sw_zend_call_method_with_1_params(
        return_value,
        swoole_timer_iterator_ce,
        &swoole_timer_iterator_ce->constructor,
        (const char *) "__construct",
        NULL,
        &zlist
    );
    zval_ptr_dtor(&zlist);
}

static PHP_FUNCTION(swoole_timer_clear)
{
    if (UNEXPECTED(!SwooleG.timer.initialized))
    {
        RETURN_FALSE;
    }
    else
    {
        zend_long id;
        swTimer_node *tnode;

        ZEND_PARSE_PARAMETERS_START(1, 1)
            Z_PARAM_LONG(id)
        ZEND_PARSE_PARAMETERS_END_EX(RETURN_FALSE);

        tnode = swTimer_get_ex(&SwooleG.timer, id, SW_TIMER_TYPE_PHP);
        RETURN_BOOL(swTimer_del_ex(&SwooleG.timer, tnode, php_swoole_timer_dtor));
    }
}

static PHP_FUNCTION(swoole_timer_clear_all)
{
    RETURN_BOOL(php_swoole_timer_clear_all());
}
