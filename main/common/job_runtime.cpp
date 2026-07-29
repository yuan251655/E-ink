#include "job_runtime.h"

#include "job_service.h"

namespace photopainter::product {
JobService& GetProductJobService() {
    static JobService service;
    return service;
}
}  // namespace photopainter::product
