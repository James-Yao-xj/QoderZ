#include "cli/commands/serve/serve.hpp"

#include "cli/output.hpp"
#include "common/log.hpp"

#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>

#if defined(__GLIBC__)
#include <malloc.h>
#endif

namespace qoderz::cli {
namespace {

void configure_allocator() {
#if defined(__GLIBC__)
  // A fixed trim threshold also disables glibc's adaptive mmap threshold. Request-sized buffers
  // then stay mmap-backed and return to the OS on free instead of accumulating in worker arenas.
  // Explicit deployment settings take precedence over this default.
  for (auto const* name : {"MALLOC_TRIM_THRESHOLD_", "MALLOC_MMAP_THRESHOLD_", "MALLOC_MMAP_MAX_",
                           "MALLOC_TOP_PAD_"}) {
    if (std::getenv(name) != nullptr) {
      return;
    }
  }
  auto const* tunables = std::getenv("GLIBC_TUNABLES");
  if (tunables != nullptr &&
      std::string_view{tunables}.find("glibc.malloc.") != std::string_view::npos) {
    return;
  }
  (void)mallopt(M_TRIM_THRESHOLD, 128 * 1024);
#endif
}

} // namespace

int run_serve(command_context& context, serve_options const& options) {
  configure_allocator();
  auto plan = plan_serve(context, options);
  if (!plan.has_value()) {
    return report_error(context.m_app, plan.error());
  }

  auto runtime = serve_runtime::create(std::move(*plan));
  if (!runtime.has_value()) {
    return report_error(context.m_app, runtime.error());
  }

  return (*runtime)->serve(context.m_app);
}

} // namespace qoderz::cli
