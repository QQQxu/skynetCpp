#include "skynet.h"

#include <string>

#include "error.h"


uint32_t SkynetQueryname(Context *ctx, std::string_view name) {
  switch (name[0]) {
    case ':':
      return std::stoul(std::string(name.substr(1)), nullptr, 16);
    case '.':
      // NOTO:
      // return  skynet_handle_findname(name + 1);
      return 0;
  }
  SkynetError(ctx->GetHandle(), "Don't support query global name " + std::string(name));
  return 0;
}
