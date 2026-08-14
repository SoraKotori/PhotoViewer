#pragma once

#include <cstddef>

namespace pv {

class PipelineObserver {
public:
    virtual void OnFrameReady(std::size_t index) = 0;
    virtual void OnFramePresented(std::size_t index) = 0;

protected:
    ~PipelineObserver() = default;
};

}  // namespace pv
