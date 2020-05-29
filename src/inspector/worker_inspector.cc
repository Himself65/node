#include "worker_inspector.h"
#include "main_thread_interface.h"
#include "util-inl.h"

#include <memory>

namespace node {
namespace inspector {
namespace {

class WorkerStartedRequest : public Request {
 public:
  WorkerStartedRequest(
      int id,
      const std::string& url,
      std::shared_ptr<node::inspector::MainThreadHandle> worker_thread,
      bool waiting)
      : id_(id),
        info_(BuildWorkerTitle(id), url, worker_thread),
        waiting_(waiting) {}
  void Call(MainThreadInterface* thread) override {
    auto manager = thread->inspector_agent()->GetWorkerManager();
    manager->WorkerStarted(id_, info_, waiting_);
  }

 private:
  static std::string BuildWorkerTitle(int id) {
    return "Worker " + std::to_string(id);
  }

  int id_;
  WorkerInfo info_;
  bool waiting_;
};


void Report(const std::unique_ptr<WorkerDelegate>& delegate,
            const WorkerInfo& info, bool waiting) {
  if (info.worker_thread)
    delegate->WorkerCreated(info.title, info.url, waiting, info.worker_thread);
}

class WorkerFinishedRequest : public Request {
 public:
  explicit WorkerFinishedRequest(int worker_id) : worker_id_(worker_id) {}

  void Call(MainThreadInterface* thread) override {
    thread->inspector_agent()->GetWorkerManager()->WorkerFinished(worker_id_);
  }

 private:
  int worker_id_;
};
}  // namespace

ParentInspectorHandle::~ParentInspectorHandle() {
  parent_thread_->Post(
      std::unique_ptr<Request>(new WorkerFinishedRequest(id_)));
}

void ParentInspectorHandle::WorkerStarted(
    std::shared_ptr<MainThreadHandle> worker_thread, bool waiting) {
  std::unique_ptr<Request> request(
      new WorkerStartedRequest(id_, url_, worker_thread, waiting));
  parent_thread_->Post(std::move(request));
}

std::unique_ptr<inspector::InspectorSession> ParentInspectorHandle::Connect(
    std::unique_ptr<inspector::InspectorSessionDelegate> delegate,
    bool prevent_shutdown) {
  return parent_thread_->Connect(std::move(delegate), prevent_shutdown);
}

void WorkerManager::WorkerFinished(int session_id) {
  children_.erase(session_id);
}

void WorkerManager::WorkerStarted(int session_id,
                                  const WorkerInfo& info,
                                  bool waiting) {
  if (info.worker_thread->Expired())
    return;
  children_.emplace(session_id, info);
  for (const auto& delegate : delegates_) {
    Report(delegate.second, info, waiting);
  }
}

std::unique_ptr<ParentInspectorHandle>
WorkerManager::NewParentHandle(int thread_id, const std::string& url) {
  bool wait = !delegates_waiting_on_start_.empty();
  return std::make_unique<ParentInspectorHandle>(thread_id, url, thread_, wait);
}

void WorkerManager::RemoveAttachDelegate(int id) {
  delegates_.erase(id);
  delegates_waiting_on_start_.erase(id);
}

std::unique_ptr<WorkerManagerEventHandle> WorkerManager::SetAutoAttach(
    std::unique_ptr<WorkerDelegate> attach_delegate) {
  int id = ++next_delegate_id_;
  delegates_[id] = std::move(attach_delegate);
  const auto& delegate = delegates_[id];
  for (const auto& worker : children_) {
    // Waiting is only reported when a worker is started, same as browser
    Report(delegate, worker.second, false);
  }
  return std::make_unique<WorkerManagerEventHandle>(shared_from_this(), id);
}

void WorkerManager::SetWaitOnStartForDelegate(int id, bool wait) {
  if (wait)
    delegates_waiting_on_start_.insert(id);
  else
    delegates_waiting_on_start_.erase(id);
}

void WorkerManagerEventHandle::SetWaitOnStart(bool wait_on_start) {
    manager_->SetWaitOnStartForDelegate(id_, wait_on_start);
}

WorkerManagerEventHandle::~WorkerManagerEventHandle() {
  manager_->RemoveAttachDelegate(id_);
}
}  // namespace inspector
}  // namespace node
