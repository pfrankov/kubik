#include "emoji_cluster.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void cluster(const char *text, const char *expected, bool expected_emoji) {
    bool emoji = !expected_emoji;
    size_t bytes = emoji_cluster_next(text, strlen(text), &emoji);
    assert(bytes == strlen(expected));
    assert(emoji == expected_emoji);
}

static void ordinary_scalars(void) {
    cluster("123", "1", false); cluster("# text", "#", false); cluster("* bold", "*", false);
    cluster("Язык", "Я", false); cluster("A\u0301", "A", false);
    cluster("☆", "☆", false); cluster("✓", "✓", false); cluster("✁", "✁", false);
    cluster("1\ufe0f", "1", false);
    bool emoji = true;
    assert(emoji_cluster_next(NULL, 2, &emoji) == 0 && !emoji);
    assert(emoji_cluster_next("", 0, NULL) == 0);
}

static void known_clusters(void) {
    cluster("😀text", "😀", true); cluster("❤️❤", "❤️", true);
    cluster("👍🏽👍", "👍🏽", true); cluster("👩‍💻A", "👩‍💻", true);
    cluster("👨‍👩‍👧‍👦!", "👨‍👩‍👧‍👦", true); cluster("🇷🇺🇨🇦", "🇷🇺", true);
    cluster("1️⃣2", "1️⃣", true); cluster("#⃣2", "#⃣", true);
    cluster("🏽x", "🏽", true);
    const char *flag = "\U0001f3f4\U000e0067\U000e0062\U000e0073\U000e0063\U000e0074\U000e007f";
    cluster(flag, flag, true);
    assert(emoji_sequence_valid(flag, strlen(flag)));
}

static void unsupported_clusters(void) {
    cluster("👩\u200dA text", "👩\u200dA", true);
    cluster("👩\u200d\u200d💻!", "👩\u200d\u200d💻", true);
    cluster("👩\u200d text", "👩\u200d", true);
    cluster("❤\u0301x", "❤\u0301", true);
    cluster("❤️\ufe0fA", "❤️\ufe0f", true);
    cluster("🏽🏻A", "🏽🏻", true);
    assert(!emoji_sequence_valid("👩\u200dA", strlen("👩\u200dA")));
    assert(!emoji_sequence_valid("❤\u0301", strlen("❤\u0301")));
    assert(!emoji_sequence_valid("🏽🏻", strlen("🏽🏻")));
    const char *flag = "\U0001f3f4\U000e0067";
    cluster(flag, flag, true);
    assert(!emoji_sequence_valid(flag, strlen(flag)));
}

static void oversized_cluster(void) {
    char text[512] = "😀";
    for (unsigned i = 0; i < 20; i++) strcat(text, "\u200d😀");
    size_t whole = strlen(text);
    strcat(text, "!");
    bool emoji;
    assert(whole > EMOJI_CLUSTER_MAX_BYTES);
    assert(emoji_cluster_next(text, strlen(text), &emoji) == whole && emoji);
    assert(!emoji_sequence_valid(text, whole));
}

static void malformed_progress(void) {
    const char text[] = {'\xf0','\x9f','A','\xff'};
    const char *cursor = text;
    uint32_t cp;
    assert(!emoji_utf8_read(&cursor, text + sizeof text, &cp) && cursor == text);
    size_t at = 0;
    while (at < sizeof text) {
        bool emoji = true;
        size_t used = emoji_cluster_next(text + at, sizeof text - at, &emoji);
        assert(used == 1 && !emoji);
        at += used;
    }
    assert(emoji_cluster_next(text, sizeof text, NULL) == 1);
    const char *bad = "\xed\xa0\x80";
    assert(!emoji_sequence_valid(bad, 3));
}

#ifdef EMOJI_OFFICIAL_VECTORS
#include "emoji_cluster_vectors.h"
static void official_sequences(void) {
    for (size_t i = 0; i < sizeof unicode_sequences / sizeof *unicode_sequences; i++) {
        const char *value = unicode_sequences[i];
        bool emoji;
        assert(emoji_cluster_next(value, strlen(value), &emoji) == strlen(value) && emoji);
        assert(emoji_sequence_valid(value, strlen(value)));
    }
    printf("Unicode 17: %zu official sequences classified atomically\n", sizeof unicode_sequences / sizeof *unicode_sequences);
}
#endif

int main(void) {
    ordinary_scalars(); known_clusters(); unsupported_clusters(); oversized_cluster(); malformed_progress();
#ifdef EMOJI_OFFICIAL_VECTORS
    official_sequences();
#endif
    puts("Emoji cluster: scalar text, complete and unsupported compounds, boundaries and malformed progress passed");
}
