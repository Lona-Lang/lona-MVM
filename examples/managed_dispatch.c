extern void *__mvm_array_malloc(unsigned long element_size,
                                unsigned long element_count,
                                unsigned long alignment);

__attribute__((noinline)) static int leaf(void *slot) {
    return slot == 0;
}

__attribute__((noinline)) static int middle(void *slot) {
    return leaf(slot);
}

int main(void) {
    unsigned char local = 7;
    void *raw = &local;
    void *values = __mvm_array_malloc(1, 4, 1);
    if (middle(raw)) {
        return 10;
    }
    if (middle(values)) {
        return 11;
    }
    return 0;
}
