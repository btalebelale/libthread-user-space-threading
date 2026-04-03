#include <stdlib.h>
#include <sys/queue.h>
#include "pool.h"
#include "log_sys.h"

typedef struct pool_node
{
    thread_m *thread;
    STAILQ_ENTRY(pool_node) link;
} pool_node;

STAILQ_HEAD(pool_head, pool_node);

struct pool
{
    struct pool_head head;
};

pool *pool_init(int cap)
{
    (void)cap;

    LOG_D("Initializing pool");

    pool *p = malloc(sizeof(*p));
    if (!p)
    {
        LOG_E("Failed to allocate pool");
        return NULL;
    }

    STAILQ_INIT(&p->head);

    LOG_D("Pool initialized successfully");

    return p;
}

void pool_free(pool *p)
{
    if (!p)
    {
        LOG_W("pool_free called with NULL");
        return;
    }

    LOG_D("Freeing pool");

    while (!STAILQ_EMPTY(&p->head))
    {
        pool_node *node = STAILQ_FIRST(&p->head);
        STAILQ_REMOVE_HEAD(&p->head, link);

        LOG_D("Freeing pool node %p", (void *)node);

        free(node);
    }

    free(p);

    LOG_D("Pool freed");
}

int is_pool_empty(pool *p)
{
    if (!p)
    {
        LOG_W("is_pool_empty called with NULL");
        return 1;
    }

    return STAILQ_EMPTY(&p->head);
}

int pool_put_last(pool *p, thread_m *t)
{
    if (!p || !t)
    {
        LOG_E("pool_put_last: invalid argument p=%p t=%p", (void *)p, (void *)t);
        return -1;
    }

    pool_node *node = malloc(sizeof(*node));
    if (!node)
    {
        LOG_E("Failed to allocate pool_node");
        return -1;
    }

    node->thread = t;

    STAILQ_INSERT_TAIL(&p->head, node, link);

    LOG_D("Thread %p added to pool", (void *)t);

    return 0;
}

thread_m *pool_remove_first(pool *p)
{
    if (!p)
    {
        LOG_E("pool_remove_first: pool is NULL");
        return NULL;
    }

    if (STAILQ_EMPTY(&p->head))
    {
        LOG_D("pool_remove_first: pool is empty");
        return NULL;
    }

    pool_node *node = STAILQ_FIRST(&p->head);
    STAILQ_REMOVE_HEAD(&p->head, link);

    thread_m *t = node->thread;

    LOG_D("Thread %p removed from pool", (void *)t);

    free(node);

    return t;
}