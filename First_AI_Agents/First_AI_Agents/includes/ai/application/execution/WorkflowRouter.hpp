#pragma once

#include "ai/application/catalog/IExecutionProfileRepository.hpp"
#include "ai/application/catalog/IModelRepository.hpp"
#include "ai/application/catalog/IProviderRepository.hpp"
#include "ai/application/execution/IWorkflowResolver.hpp"

#include <unordered_map>

namespace ai::application {

/**
 * Concrete provider-neutral registry/router. Composition roots (currently
 * only BackendRuntime) register one IWorkflowRunner per workflowId - the
 * Mock runner under kMockWorkflowId, and (only when a real provider adapter
 * is compiled in) the production runner under kExecutionProfileWorkflowId -
 * then hand this object to RunManager as an IWorkflowResolver&.
 *
 * WorkflowRouter never owns a runner: registerRunner() stores a raw
 * reference. Registered runners must outlive both this router and every
 * RunManager built on it - BackendRuntime's member declaration order
 * guarantees this the same way it already guarantees storage outlives the
 * services built on it.
 */
class WorkflowRouter final : public IWorkflowResolver {
public:
    WorkflowRouter(
        IExecutionProfileRepository& profiles,
        IProviderRepository& providers,
        IModelRepository& models);

    void registerRunner(std::string workflowId, IWorkflowRunner& runner);

    [[nodiscard]] common::Result<ResolvedExecutionPlan> resolvePlan(
        const StartRunRequest& request) const override;
    common::Result<std::reference_wrapper<IWorkflowRunner>> runnerFor(
        const std::string& workflowId) override;

private:
    IExecutionProfileRepository& profiles_;
    IProviderRepository& providers_;
    IModelRepository& models_;
    std::unordered_map<std::string, IWorkflowRunner*> runners_;
};

} // namespace ai::application
