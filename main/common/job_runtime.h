#pragma once

namespace photopainter::product {
class JobService;

// The product API and background workers share one bounded, thread-safe job
// registry. It intentionally remains RAM-only; durable media state lives on TF.
JobService& GetProductJobService();
}  // namespace photopainter::product
