#pragma once

namespace rf {
struct AdapterInfo;
}

namespace rf::vendor {

void EnrichAdapter(AdapterInfo& info);

bool NvapiAvailable();
bool AmdDriverAvailable();

}
