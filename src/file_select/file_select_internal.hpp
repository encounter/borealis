#pragma once

#include "borealis/file_select.hpp"

namespace borealis::file_select::detail {

void complete(Callback callback, Result result);
void reject(Callback callback, Result result);
Result result_from_file_list(const char* const* fileList, const char* error);

void open_macos_folder(FolderOptions options, Callback callback);
void open_ios_file(FileOptions options, Callback callback);
void open_ios_folder(FolderOptions options, Callback callback);
void open_android_folder(FolderOptions options, Callback callback);

}  // namespace borealis::file_select::detail
