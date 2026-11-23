/* Offline Phase 3-I PREPARE and settings validation. */
#include "../src/workspace-animation.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#define OWNER 0x400001UL
#define EPOCH 3UL
static unsigned long words[WORKSPACE_ANIMATION_WORDS + 1];
static unsigned long txn(unsigned long count) {
    unsigned long header[] = {1, OWNER, EPOCH, 7, 0, 1, 1234, count};
    memcpy(words, header, sizeof(header));
    for (unsigned long i = 0; i < count; i++) words[WORKSPACE_ANIMATION_HEADER + i] = 0x200001 + i;
    return WORKSPACE_ANIMATION_HEADER + count;
}
int main(void) {
    unsigned long s[] = {1, OWNER, 250};
    assert(workspace_settings_valid(s, 3, 0, OWNER));
    assert(!workspace_settings_valid(s, 3, 0, OWNER + 1));
    assert(!workspace_settings_valid(s, 3, 1, OWNER));
    assert(!workspace_settings_valid(s, 2, 0, OWNER));
    s[2] = 251; assert(!workspace_settings_valid(s, 3, 0, OWNER));
    s[2] = 0; s[0] = 2; assert(!workspace_settings_valid(s, 3, 0, OWNER));
    WorkspaceTxn t;
    unsigned long n = txn(2);
    assert(workspace_txn_decode(&t, words, n, 0, OWNER, EPOCH));
    assert(t.sequence == 7 && t.from == 0 && t.to == 1 && t.count == 2 && t.members[1] == 0x200002);
    assert(!workspace_txn_decode(&t, words, n, 0, OWNER, EPOCH + 1)); /* stale epoch */
    assert(!workspace_txn_decode(&t, words, n, 0, OWNER + 1, EPOCH)); /* other owner */
    assert(!workspace_txn_decode(&t, words, n, 1, OWNER, EPOCH));     /* truncated */
    assert(!workspace_txn_decode(&t, words, n - 1, 0, OWNER, EPOCH)); /* count mismatch */
    words[5] = 0; assert(!workspace_txn_decode(&t, words, n, 0, OWNER, EPOCH)); /* from == to */
    n = txn(2); words[3] = 0; assert(!workspace_txn_decode(&t, words, n, 0, OWNER, EPOCH));
    n = txn(2); words[9] = words[8]; assert(!workspace_txn_decode(&t, words, n, 0, OWNER, EPOCH));
    n = txn(2); words[9] = 0; assert(!workspace_txn_decode(&t, words, n, 0, OWNER, EPOCH));
#if ULONG_MAX > UINT32_MAX
    n = txn(2); words[8] |= 1UL << 40; assert(!workspace_txn_decode(&t, words, n, 0, OWNER, EPOCH));
#endif
    n = txn(WORKSPACE_ANIMATION_MEMBERS);
    assert(workspace_txn_decode(&t, words, n, 0, OWNER, EPOCH));
    n = txn(WORKSPACE_ANIMATION_MEMBERS + 1);
    assert(!workspace_txn_decode(&t, words, n, 0, OWNER, EPOCH));
    n = txn(0);
    assert(workspace_txn_decode(&t, words, n, 0, OWNER, EPOCH) && t.count == 0);
    assert(workspace_sequence_newer(2, 1) && !workspace_sequence_newer(1, 2));
    assert(workspace_sequence_newer(1, UINT32_MAX) && !workspace_sequence_newer(0, 1));
    assert(!workspace_sequence_newer(5, 5) && workspace_sequence_newer(1, 0));
    puts("PASS: Phase 3-I settings cap, PREPARE owner/epoch/length/member checks, sequence order");
    return 0;
}
