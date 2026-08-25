#pragma once

#include "borealis/update.hpp"

namespace borealis::update::detail {

http::Request make_request(const AppInfo& info, const Options& options);
Result result_from_response(
    http::Result response, std::string_view currentVersion, bool includePrereleases);

}  // namespace borealis::update::detail
