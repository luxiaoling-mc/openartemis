// openartemis_test — AI / regression-verification build of the app host.
//
// Compiles the SAME translation unit as the plain user binary (main.cpp)
// with OA_TEST_BUILD=1, which adds the test-only hooks the user build
// excludes: the smoke probes (title-button plateau metrics, OA_TXT_SMOKE
// walk), the diagnostic print streams (pointer observer, per-frame
// milestones) and the env hooks OA_UI_OUT / OA_R10_* / OA_DEBUG_* /
// OA_STATE_LOG / OA_GLYPH_LOG / OA_P1B_DEBUG / OA_VIDEO_DEMO /
// OA_VIDEO_DEBUG. The autodrive journeys themselves (OA_AUTODRIVE=exit |
// title | help | conf | r10save | r10t | r10blog | qld | d38 | nmfg | scale)
// are implemented in the second TU, app_test_drive.cpp (same target; entry
// points declared in app_host.h).
//
// See docs/TESTING.md. Behaviour outside those facilities is identical to
// `openartemis`.
#define OA_TEST_BUILD 1
#include "main.cpp"
