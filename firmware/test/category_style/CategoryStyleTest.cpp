#include "testutil.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

#include "store/PlannerStore.h"
#include "ui/CategoryStyle.h"

using store::Category;
using ui::CategoryVisual;
using ui::styleFor;

namespace {

// If the Lucide icons directory can't be located (e.g. the freeink-sdk
// submodule isn't populated in this checkout), fall back to checking
// iconSlug against the known-good hardcoded list instead of failing the
// whole test on an environment issue unrelated to CategoryStyle's own
// logic -- called out here rather than silently downgrading elsewhere.
bool iconsDirLooksPopulated(const std::string& dir) {
  std::ifstream probe(dir + "/circle.svg");
  return probe.good();
}

bool fileExists(const std::string& path) {
  std::ifstream f(path);
  return f.good();
}

}  // namespace

int main(int argc, char** argv) {
  const Category kAll[8] = {Category::Work,   Category::Break,  Category::Chore,   Category::Health,
                             Category::Social, Category::School, Category::Personal, Category::Other};
  static const char* kKnownGoodSlugs[8] = {"briefcase", "coffee",         "spray-can", "heart-pulse",
                                            "users",     "graduation-cap", "user",      "circle-ellipsis"};

  bool liveCheck = argc > 1 && iconsDirLooksPopulated(argv[1]);
  if (!liveCheck) {
    std::fprintf(stderr,
                  "CategoryStyleTest: Lucide icons directory not found/populated, "
                  "falling back to hardcoded slug-list check\n");
  }

  for (size_t i = 0; i < 8; i++) {
    const CategoryVisual& v = styleFor(kAll[i]);
    CHECK(v.iconSlug != nullptr);
    CHECK(std::strlen(v.iconSlug) > 0);
    CHECK(std::strcmp(v.iconSlug, kKnownGoodSlugs[i]) == 0);
    if (liveCheck) {
      std::string path = std::string(argv[1]) + "/" + v.iconSlug + ".svg";
      CHECK(fileExists(path));
    }
  }

  // styleFor() returns a distinct iconSlug for each of the 8 categories.
  for (size_t i = 0; i < 8; i++) {
    for (size_t j = i + 1; j < 8; j++) {
      CHECK(std::strcmp(styleFor(kAll[i]).iconSlug, styleFor(kAll[j]).iconSlug) != 0);
    }
  }

  // Stable/idempotent: same category -> same values across calls.
  const CategoryVisual& first = styleFor(Category::Work);
  const CategoryVisual& second = styleFor(Category::Work);
  CHECK(first.iconSlug == second.iconSlug);  // same pointer identity (static table)
  CHECK(first.dither == second.dither);

  // Defensive clamp: an out-of-range value behaves like Other rather than
  // reading out of bounds.
  const CategoryVisual& clamped = styleFor(static_cast<Category>(200));
  const CategoryVisual& other = styleFor(Category::Other);
  CHECK(std::strcmp(clamped.iconSlug, other.iconSlug) == 0);
  CHECK(clamped.dither == other.dither);

  std::printf("CategoryStyleTest: all assertions passed%s\n", liveCheck ? "" : " (fallback mode)");
  return 0;
}
