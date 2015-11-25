#pragma once

#include <memory>
#include "IRandomizer.h"

namespace Microsoft { namespace MSR { namespace CNTK {
    class RollingWindowRandomizer : public IRandomizer
    {
        int rank_;
        int numberOfWorkers_;
        int rollingWindowSize_;

    public:
        RollingWindowRandomizer(std::shared_ptr<ISource> source, int rank, int numberOfWorkers, int rollingWindowSize)
            : IRandomizer(source)
            , rank_(rank)
            , numberOfWorkers_(numberOfWorkers)
            , rollingWindowSize_(rollingWindowSize)
        {}
    };
}}}