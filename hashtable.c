/* Copyright (C) 2004 Christopher Clark <firstname.lastname@cl.cam.ac.uk> */
/*
* Copyright (c) 2002, Christopher Clark
* All rights reserved.
* 
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions
* are met:
* 
* * Redistributions of source code must retain the above copyright
* notice, this list of conditions and the following disclaimer.
* 
* * Redistributions in binary form must reproduce the above copyright
* notice, this list of conditions and the following disclaimer in the
* documentation and/or other materials provided with the distribution.
* 
* * Neither the name of the original author; nor the names of any contributors
* may be used to endorse or promote products derived from this software
* without specific prior written permission.
* 
* 
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
* "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
* LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
* A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER
* OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
* EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
* PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
* PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
* LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
* NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
* SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

/* NJH: I've added the changes from http://fisheye.freeswitch.org/browse/OpenZAP/tags/v427/src/hashtable.c?r1=3&r2=8&u=24&ignore=&k= */

#include "hashtable.h"
#include "hashtable_private.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
/*#include "../MALLOC/includes/MALLOC.h"*/
#define FREE(x)	free(x)
#define MALLOC(x)	malloc(x)
#define REALLOC(x, y)	realloc(x, y)

/*
Credit for primes table: Aaron Krowne
 http://br.endernet.org/~akrowne/
 http://planetmath.org/encyclopedia/GoodHashTablePrimes.html
*/
static const unsigned int primes[] = {
53, 97, 193, 389,
769, 1543, 3079, 6151,
12289, 24593, 49157, 98317,
196613, 393241, 786433, 1572869,
3145739, 6291469, 12582917, 25165843,
50331653, 100663319, 201326611, 402653189,
805306457, 1610612741
};
const unsigned int prime_table_length = sizeof(primes)/sizeof(primes[0]);
const float max_load_factor = 0.65;

/*****************************************************************************/
struct hashtable *
create_hashtable(unsigned int minsize,
                 unsigned int (*hashf) (const void*),
                 int (*eqf) (const void*,const void*))
{
    struct hashtable *h;
    unsigned int pindex, size = primes[0];
    /* Check requested hashtable isn't too large */
    if (minsize > (1u << 30)) return NULL;
    /* Enforce size as prime */
    for (pindex=0; pindex < prime_table_length; pindex++) {
        if (primes[pindex] > minsize) { size = primes[pindex]; break; }
    }
    h = (struct hashtable *)MALLOC(sizeof(struct hashtable));
    if (NULL == h) return NULL; /*oom*/
    h->table = (struct entry **)MALLOC(sizeof(struct entry*) * size);
    if (NULL == h->table) { FREE(h); return NULL; } /*oom*/
    memset(h->table, 0, size * sizeof(struct entry *));
    h->tablelength  = size;
    h->primeindex   = pindex;
    h->entrycount   = 0;
    h->hashfn       = hashf;
    h->eqfn         = eqf;
    h->loadlimit    = (unsigned int) ceil(size * max_load_factor);
    return h;
}

/*****************************************************************************/
unsigned int
hash(struct hashtable *h, const void *k)
{
    /* Aim to protect against poor hash functions by adding logic here
     * - logic taken from java 1.4 hashtable source */
    unsigned int i = h->hashfn(k);
    i += ~(i << 9);
    i ^=  ((i >> 14) | (i << 18)); /* >>> */
    i +=  (i << 4);
    i ^=  ((i >> 10) | (i << 22)); /* >>> */
    return i;
}

/*****************************************************************************/
static int
hashtable_expand(struct hashtable *h)
{
    /* Double the size of the table to accomodate more entries */
    struct entry **newtable;
    struct entry *e;
    unsigned int newsize, i, index_;
    /* Check we're not hitting max capacity */
    if (h->primeindex == (prime_table_length - 1)) return 0;
    newsize = primes[++(h->primeindex)];

    newtable = (struct entry **)MALLOC(sizeof(struct entry*) * newsize);
    if (NULL != newtable)
    {
        memset(newtable, 0, newsize * sizeof(struct entry *));
        /* This algorithm is not 'stable'. ie. it reverses the list
         * when it transfers entries between the tables */
        for (i = 0; i < h->tablelength; i++) {
            while (NULL != (e = h->table[i])) {
                h->table[i] = e->next;
                index_ = indexFor(newsize,e->h);
                e->next = newtable[index_];
                newtable[index_] = e;
            }
        }
        FREE(h->table);
        h->table = newtable;
    }
    /* Plan B: realloc instead */
    else 
    {
        newtable = (struct entry **) REALLOC(h->table, newsize * sizeof(struct entry *));
        if (NULL == newtable) { (h->primeindex)--; return 0; }
        h->table = newtable;
        memset(newtable[h->tablelength], 0, newsize - h->tablelength);
        for (i = 0; i < h->tablelength; i++) {
	    struct entry **pE;
            for (pE = &(newtable[i]), e = *pE; e != NULL; e = *pE) {
                index_ = indexFor(newsize,e->h);
                if (index_ == i)
                {
                    pE = &(e->next);
                }
                else
                {
                    *pE = e->next;
                    e->next = newtable[index_];
                    newtable[index_] = e;
                }
            }
        }
    }
    h->tablelength = newsize;
    h->loadlimit   = (unsigned int) ceil(newsize * max_load_factor);
    return -1;
}

/*****************************************************************************/
unsigned int
hashtable_count(struct hashtable *h)
{
    return h->entrycount;
}

/*****************************************************************************/
int
hashtable_insert(struct hashtable *h, void *k, void *v)
{
    /* This method allows duplicate keys - but they shouldn't be used */
    unsigned int index_;
    struct entry *e;
    if (++(h->entrycount) > h->loadlimit)
    {
        /* Ignore the return value. If expand fails, we should
         * still try cramming just this value into the existing table
         * -- we may not have memory for a larger table, but one more
         * element may be ok. Next time we insert, we'll try expanding again.*/
        hashtable_expand(h);
    }
    e = (struct entry *)MALLOC(sizeof(struct entry));
    if (NULL == e) { --(h->entrycount); return 0; } /*oom*/
    e->h = hash(h,k);
    index_ = indexFor(h->tablelength,e->h);
    e->k = k;
    e->v = v;
    e->next = h->table[index_];
    h->table[index_] = e;
    return -1;
}

/*****************************************************************************/


/** 
 * Returns value associated with key
 */
void * hashtable_search(struct hashtable *h, const void *k)
{
    struct entry *e;
    unsigned int hashvalue, index_;
	if (h==NULL){
		/* Check that the hashtable does exist. */
		printf("Internal error: cannot search into an NULL hashtable !\n");
		exit(-1);
	}
    hashvalue = hash(h,k);
    index_ = indexFor(h->tablelength,hashvalue);
    e = h->table[index_];
    while (NULL != e)
    {
        /* Check hash value to short circuit heavier comparison */
        if ((hashvalue == e->h) && (h->eqfn(k, e->k))) 
		{
			return e->v;
		}
			
        e = e->next;
    }
    return NULL;
}

/*****************************************************************************/
void * /* returns value associated with key */
hashtable_remove(struct hashtable *h, void *k)
{
    /* TODO: consider compacting the table when the load factor drops enough,
     *       or provide a 'compact' method. */

    struct entry *e;
    struct entry **pE;
    void *v;
    unsigned int hashvalue, index_;

    hashvalue = hash(h,k);
    index_ = indexFor(h->tablelength,hash(h,k));
    pE = &(h->table[index_]);
    e = *pE;
    while (NULL != e)
    {
        /* Check hash value to short circuit heavier comparison */
        if ((hashvalue == e->h) && (h->eqfn(k, e->k)))
        {
            *pE = e->next;
            h->entrycount--;
            v = e->v;
            freekey(e->k);
            FREE(e);
            return v;
        }
        pE = &(e->next);
        e = e->next;
    }
    return NULL;
}

/*****************************************************************************/
/* destroy */
void hashtable_destroy(struct hashtable *h, int free_values)
{
    unsigned int i;
    struct entry *e, *f;
    struct entry **table = h->table;
    if (free_values)
    {
        for (i = 0; i < h->tablelength; i++)
        {
            e = table[i];
            while (NULL != e)
            { f = e; e = e->next; freekey(f->k); FREE(f->v); FREE(f); }
        }
    }
    else
    {
        for (i = 0; i < h->tablelength; i++)
        {
            e = table[i];
            while (NULL != e)
            { f = e; e = e->next; freekey(f->k); FREE(f); }
        }
    }
    FREE(h->table);
    FREE(h);
}

