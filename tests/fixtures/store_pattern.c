/* fixture: store_pattern
 * Simulates a simplified store CRUD pattern.
 * open → insert → find → close
 * find → ht_get (hash table lookup)
 * insert → ht_set (hash table insert) */

typedef struct {
    int id;
    char *name;
} Record;

typedef struct {
    Record *records;
    int count;
} Store;

Store *store_open(const char *path) {
    Store *s = (Store *)malloc(sizeof(Store));
    s->records = NULL;
    s->count = 0;
    return s;
}

void store_close(Store *s) {
    free(s->records);
    free(s);
}

Record *ht_get(Store *s, int id) {
    for (int i = 0; i < s->count; i++) {
        if (s->records[i].id == id) return &s->records[i];
    }
    return NULL;
}

int ht_set(Store *s, Record rec) {
    s->records = (Record *)realloc(s->records, (s->count + 1) * sizeof(Record));
    s->records[s->count] = rec;
    return s->count++;
}

Record *store_find(Store *s, int id) {
    return ht_get(s, id);
}

int store_insert(Store *s, Record rec) {
    return ht_set(s, rec);
}

void do_work(const char *path) {
    Store *s = store_open(path);
    Record r = {1, "test"};
    store_insert(s, r);
    store_find(s, 1);
    store_close(s);
}
