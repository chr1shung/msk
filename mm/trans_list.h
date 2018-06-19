#ifndef _TRANS_LIST_H
#define _TRANS_LIST_H

struct trans_list_head {
    struct trans_list_head *next, *next_tail ;
} ;

struct trans_list_log {
    struct trans_list_head *last_new ;
    size_t last_addr ;
} ;

#define TRANS_LIST_HEAD_INIT(name) { &(name), &(name) }

#define TRANS_LIST_HEAD(name,log) \
        struct trans_list_head name = TRANS_LIST_HEAD_INIT(name) ; \
        struct trans_list_log log 

#define trans_entry(ptr, type, member) \
    container_of(ptr, type, member)

#define trans_first_entry(ptr, type, member) \
    trans_entry((ptr)->next, type, member)

#define trans_next_entry(pos, member) \
    trans_entry((pos)->member.next, typeof(*(pos)), member)

#define trans_prepare_entry(pos, head, member) \
    ((pos) ? : trans_entry(head, typeof(*pos), member))

#define trans_list_for_each_entry_continue(pos, head, member)   \
    for (pos = trans_next_entry(pos, member);           \
        &pos->member != (head);                     \
        pos = trans_next_entry(pos, member))

static inline void INIT_TRANS_LIST_HEAD(struct trans_list_head *list)
{
    list->next = list;
    list->next_tail = list;
}

static inline void trans_list_add_tail(
        struct trans_list_head *new,
        struct trans_list_head *head, 
        struct trans_list_log  *log, 
        size_t addr)
{

    struct trans_list_head *last = log->last_new ;

    // is empty?
    if( head->next == head )
        last = head ;
    // reset addr?
    else if( addr < log->last_addr )
        last = head ;

    // reach the end?
    if( last->next_tail == head ){
        *new  = (struct trans_list_head){head, head} ;
        *last = (struct trans_list_head){new, new} ;
    }
    else{
        *new = *(last->next_tail) ;
        last->next_tail->next = new ;
        last->next_tail = new ;
    }

    // update log
    *log = (struct trans_list_log){new, addr} ;
}

static inline int is_last(const struct trans_list_head *list,
                            const struct trans_list_head *head)
{
    return list->next == head;
}

#endif
