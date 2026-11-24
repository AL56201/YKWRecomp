// nn::ngc::ProfanityFilter on the host. The real one reads the console's NgWord system data (0100000000000823), which
// isn't part of the game: the SDK then aborts as the player confirms their name. Every word passes here.
#include "../hle.h"

HLE_FUNC("_ZN2nn3ngc15ProfanityFilterC1Ev", ngc_ctor) { (void)c; }
HLE_FUNC("_ZN2nn3ngc15ProfanityFilterD1Ev", ngc_dtor) { (void)c; }
HLE_FUNC("_ZN2nn3ngc15ProfanityFilter10InitializeEPvm", ngc_initialize) { c->x[0] = RES_OK; }
HLE_FUNC("_ZN2nn3ngc15ProfanityFilter8FinalizeEv", ngc_finalize) { c->x[0] = RES_OK; }

// CheckProfanityWords(this, u32 *results, u32 pattern_flags, const char **words, size_t count): a bit set in
// results[i] marks word i as matching a pattern; none do.
HLE_FUNC("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjjPPKcm", ngc_check_words) {
    uint32_t *results = (uint32_t *)(uintptr_t)c->x[1];
    uint64_t count = c->x[4];
    if (results)
        for (uint64_t i = 0; i < count && i < 4096; i++) results[i] = 0;
    c->x[0] = RES_OK;
}
