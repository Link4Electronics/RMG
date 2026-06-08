#include <stdlib.h>
#include "Recompile.h"
#include "Recomp-Cache.h"

static inline PowerPC_func_node** _find(PowerPC_func_node** node, unsigned int addr){
    while(*node){
        if(addr < (*node)->function->start_address)
            node = &(*node)->left;
        else if(addr >= (*node)->function->end_address)
            node = &(*node)->right;
        else
            break;
    }
    return node;
}

PowerPC_func* find_func(PowerPC_func_node** root, unsigned int addr){
    PowerPC_func_node* node = *_find(root, addr);
    return node ? node->function : NULL;
}

void insert_func(PowerPC_func_node** root, PowerPC_func* func){
    PowerPC_func_node** node = _find(root, func->start_address);
    if(*node) return;

    *node = MetaCache_Alloc(sizeof(PowerPC_func_node));
    (*node)->function = func;
    (*node)->left = (*node)->right = NULL;
}

void remove_node(PowerPC_func_node** node){
    PowerPC_func_node* old = *node;
    if(!(*node)->left)
    {
        *node = (*node)->right;
        MetaCache_Free(old);
    }
    else if(!(*node)->right)
    {
        *node = (*node)->left;
        MetaCache_Free(old);
    }
    else
    {
        PowerPC_func_node** pre;
        for(pre = &(*node)->left; (*pre)->right; pre = &(*pre)->right);

        PowerPC_func* tmp = (*pre)->function;
        (*pre)->function=(*node)->function;
        (*node)->function=tmp;

        remove_node(pre);
    }
}

void remove_func(PowerPC_func_node** root, PowerPC_func* func){
    PowerPC_func_node** node = _find(root, func->start_address);
    if(!*node) return;

    remove_node(node);
}
