#include "cli/commands/serve/serve.hpp"

#include "cli/output.hpp"
#include "common/log.hpp"

#include <string>
#include <utility>

namespace qoderz::cli {

int run_serve(command_context& context, serve_options const& options) {
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
