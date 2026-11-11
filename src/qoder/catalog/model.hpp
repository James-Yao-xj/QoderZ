#pragma once

#include <string>
#include <vector>

namespace qoderz::qoder::catalog {

/// One model a site offers, as the CLI, the OpenAI facade and the chat client see it.
///
/// The descriptor is a flattened view of the upstream JSON: the first fields drive request shape,
/// the pricing fields only drive display. `m_raw_json` keeps the original object compact, because a
/// chat request has to send the model object back as `model_config`; re-serialising the descriptor
/// would lose fields this layer does not understand.
struct model_info {
  std::string m_key;
  std::string m_name;
  std::string m_source;
  int m_context = 0;
  int m_default_window = 0;
  std::vector<int> m_windows;
  bool m_images = false;
  bool m_thinks = false;
  bool m_always_thinks = false;
  std::vector<std::string> m_efforts;
  std::string m_default_effort;
  double m_rate = 0;
  double m_rate_was = 0;
  bool m_free = false;
  std::string m_raw_json;
};

} // namespace qoderz::qoder::catalog
