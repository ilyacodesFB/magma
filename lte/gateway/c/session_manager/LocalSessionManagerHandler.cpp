/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */
#include <chrono>
#include <thread>

#include <google/protobuf/util/time_util.h>

#include "LocalSessionManagerHandler.h"
#include "magma_logging.h"

using grpc::Status;

namespace magma {

const std::string LocalSessionManagerHandlerImpl::hex_digit_ =
        "0123456789abcdef";

LocalSessionManagerHandlerImpl::LocalSessionManagerHandlerImpl(
  LocalEnforcer *enforcer,
  SessionCloudReporter *reporter):
  enforcer_(enforcer),
  reporter_(reporter),
  current_epoch_(0),
  reported_epoch_(0),
  retry_timeout_(1)
{
}

void LocalSessionManagerHandlerImpl::ReportRuleStats(
  ServerContext *context,
  const RuleRecordTable *request,
  std::function<void(Status, Void)> response_callback)
{
  auto &request_cpy = *request;
  MLOG(MDEBUG) << "Aggregating " << request_cpy.records_size() << " records";
  enforcer_->get_event_base().runInEventBaseThread([this, request_cpy]() {
    enforcer_->aggregate_records(request_cpy);
    check_usage_for_reporting();
  });
  reported_epoch_ = request_cpy.epoch();
  if (is_pipelined_restarted()) {
    MLOG(MDEBUG) << "Pipelined has been restarted, attempting to sync flows";
    restart_pipelined(reported_epoch_);
    // Set the current epoch right away to prevent double setup call requests
    current_epoch_ = reported_epoch_;
  }
  response_callback(Status::OK, Void());
}

void LocalSessionManagerHandlerImpl::check_usage_for_reporting()
{
  auto request = enforcer_->collect_updates();
  if (request.updates_size() == 0 && request.usage_monitors_size() == 0) {
    return; // nothing to report
  }
  MLOG(MDEBUG) << "Sending " << request.updates_size()
               << " charging updates and " << request.usage_monitors_size()
               << " monitor updates to OCS and PCRF";

  // report to cloud
  reporter_->report_updates(
    request, [this, request](Status status, UpdateSessionResponse response) {
      if (!status.ok()) {
        enforcer_->reset_updates(request);
        MLOG(MERROR) << "Update of size " << request.updates_size()
                     << " to OCS failed entirely: " << status.error_message();
      } else {
        MLOG(MDEBUG) << "Received updated responses from OCS and PCRF";
        enforcer_->update_session_credit(response);
        // Check if we need to report more updates
        check_usage_for_reporting();
      }
    });
}

bool LocalSessionManagerHandlerImpl::is_pipelined_restarted()
{
  // If 0 also setup pipelined because it always waits for setup instructions
  if (current_epoch_ == 0 || current_epoch_ != reported_epoch_) {
    return true;
  }
  return false;
}

void LocalSessionManagerHandlerImpl::handle_setup_callback(
  const std::uint64_t &epoch,
  Status status,
  SetupFlowsResult resp)
{
  using namespace std::placeholders;
  if (!status.ok()) {
    MLOG(MERROR) << "Could not setup pipelined, rpc failed with: "
                 << status.error_message() << ", retrying pipelined setup.";

    enforcer_->get_event_base().runInEventBaseThread([=] {
      enforcer_->get_event_base().timer().scheduleTimeoutFn(
        std::move([=] {
          enforcer_->setup(epoch,
            std::bind(&LocalSessionManagerHandlerImpl::handle_setup_callback,
                      this, epoch, _1, _2));
        }),
        retry_timeout_);
    });
  }

  if (resp.result() == resp.OUTDATED_EPOCH) {
    MLOG(MWARNING) << "Pipelined setup call has outdated epoch, abandoning.";
  } else if (resp.result() == resp.FAILURE) {
    MLOG(MWARNING) << "Pipelined setup failed, retrying pipelined setup "
                      "for epoch " << epoch;
    enforcer_->get_event_base().runInEventBaseThread([=] {
      enforcer_->get_event_base().timer().scheduleTimeoutFn(
        std::move([=] {
          enforcer_->setup(epoch,
            std::bind(&LocalSessionManagerHandlerImpl::handle_setup_callback,
                      this, epoch, _1, _2));
        }),
        retry_timeout_);
    });
  } else {
    MLOG(MDEBUG) << "Successfully setup pipelined.";
  }
}

bool LocalSessionManagerHandlerImpl::restart_pipelined(
  const std::uint64_t &epoch)
{
  using namespace std::placeholders;
  enforcer_->get_event_base().runInEventBaseThread([this, epoch]() {
    enforcer_->setup(epoch,
      std::bind(&LocalSessionManagerHandlerImpl::handle_setup_callback,
                this, epoch, _1, _2));
  });
  return true;
}

static CreateSessionRequest copy_wifi_session_info2create_req(
  const LocalCreateSessionRequest *request,
  const std::string &sid)
{
  CreateSessionRequest create_request;

  create_request.mutable_subscriber()->CopyFrom(request->sid());
  create_request.set_session_id(sid);
  create_request.set_ue_ipv4(request->ue_ipv4());
  create_request.set_apn(request->apn());
  create_request.set_imei(request->imei());
  create_request.set_msisdn(request->msisdn());
  create_request.set_hardware_addr(request->hardware_addr());

  return create_request;
}

static CreateSessionRequest copy_session_info2create_req(
  const LocalCreateSessionRequest *request,
  const std::string &sid)
{
  CreateSessionRequest create_request;

  create_request.mutable_subscriber()->CopyFrom(request->sid());
  create_request.set_session_id(sid);
  create_request.set_ue_ipv4(request->ue_ipv4());
  create_request.set_spgw_ipv4(request->spgw_ipv4());
  create_request.set_apn(request->apn());
  create_request.set_msisdn(request->msisdn());
  create_request.set_imei(request->imei());
  create_request.set_plmn_id(request->plmn_id());
  create_request.set_imsi_plmn_id(request->imsi_plmn_id());
  create_request.set_user_location(request->user_location());
  create_request.set_hardware_addr(request->hardware_addr());
  if (request->has_qos_info()) {
    create_request.mutable_qos_info()->CopyFrom(request->qos_info());
  }

  return create_request;
}

void LocalSessionManagerHandlerImpl::CreateSession(
  ServerContext *context,
  const LocalCreateSessionRequest *request,
  std::function<void(Status, LocalCreateSessionResponse)> response_callback)
{
  auto imsi = request->sid().id();
  auto sid = id_gen_.gen_session_id(imsi);
  auto mac_addr = convert_mac_addr_to_str(request->hardware_addr());
  SessionState::Config cfg = {.ue_ipv4 = request->ue_ipv4(),
                              .spgw_ipv4 = request->spgw_ipv4(),
                              .msisdn = request->msisdn(),
                              .apn = request->apn(),
                              .imei = request->imei(),
                              .plmn_id = request->plmn_id(),
                              .imsi_plmn_id = request->imsi_plmn_id(),
                              .user_location = request->user_location(),
                              .rat_type = request->rat_type(),
                              .mac_addr = mac_addr,
                              .hardware_addr = request->hardware_addr(),
                              .radius_session_id = request->radius_session_id(),
                              .bearer_id = request->bearer_id()};

  SessionState::QoSInfo qos_info = {.enabled = request->has_qos_info()};
  if (request->has_qos_info()) {
    qos_info.qci = request->qos_info().qos_class_id();
  }
  cfg.qos_info = qos_info;

  if (enforcer_->is_imsi_duplicate(imsi)) {
    if (enforcer_->is_session_duplicate(imsi, cfg)) {
      MLOG(MINFO) << "Found completely duplicated session with IMSI " << imsi
                  << ", not creating session";
      return;
    }
    MLOG(MINFO) << "Found session with the same IMSI " << imsi
                << ", terminating the old session";
    EndSession(
      context,
      &request->sid(),
      [&](grpc::Status status, LocalEndSessionResponse response) {
        return;
      });
  }
  send_create_session(
    copy_session_info2create_req(request, sid),
    imsi, sid, cfg, response_callback);
}

void LocalSessionManagerHandlerImpl::send_create_session(
  const CreateSessionRequest &request,
  const std::string &imsi,
  const std::string &sid,
  const SessionState::Config &cfg,
  std::function<void(grpc::Status, LocalCreateSessionResponse)> response_callback)
{
  reporter_->report_create_session(
    request,
    [this, imsi, sid, cfg, response_callback](
      Status status, CreateSessionResponse response) {
      if (status.ok()) {
        bool success = enforcer_->init_session_credit(imsi, sid, cfg, response);
        if (!success) {
          MLOG(MERROR) << "Failed to init session in Usage Monitor "
                       << "for IMSI " << imsi;
          status =
            Status(
              grpc::FAILED_PRECONDITION, "Failed to initialize session");
        } else {
          MLOG(MINFO) << "Successfully initialized new session "
                      << "in sessiond for subscriber " << imsi;
        }
      } else {
        MLOG(MERROR) << "Failed to initialize session in OCS for IMSI "
                     << imsi << ": " << status.error_message();
      }
      response_callback(status, LocalCreateSessionResponse());
    });
}

std::string LocalSessionManagerHandlerImpl::convert_mac_addr_to_str(
        const std::string& mac_addr)
{
  std::string res;
  auto l = mac_addr.length();
  if (l == 0) {
      return res;
  }
  res.reserve(l*3-1);
  for (int i = 0; i < l; i++) {
    if (i > 0) {
      res.push_back(':');
    }
    unsigned char c = mac_addr[i];
    res.push_back(hex_digit_[c >> 4]);
    res.push_back(hex_digit_[c & 0x0F]);
  }
  return res;
}

static void report_termination(
  SessionCloudReporter &reporter,
  const SessionTerminateRequest &term_req)
{
  reporter.report_terminate_session(
    term_req,
    [&reporter, term_req](Status status, SessionTerminateResponse response) {
      if (!status.ok()) {
        MLOG(MERROR) << "Failed to terminate session in controller for "
                        "subscriber "
                     << term_req.sid() << ": " << status.error_message();
      } else {
        MLOG(MDEBUG) << "Termination successful in controller for "
                        "subscriber "
                     << term_req.sid();
      }
    });
}

/**
 * EndSession completes the entire termination procedure with the OCS & PCRF.
 * The process for session termination is as follows:
 * 1) Start termination process. Enforcer sends delete flow request to Pipelined
 * 2) Enforcer continues to collect usages until its flows are no longer
 *    included in the report (flow deleted in Pipelined) or a specified timeout
 * 3) Asynchronously report usages to cloud in termination requests to
 *    OCS & PCRF
 * 4) Remove the terminated session from being tracked locally, no matter cloud
 *    termination succeeds or not
 */
void LocalSessionManagerHandlerImpl::EndSession(
  ServerContext *context,
  const SubscriberID *request,
  std::function<void(Status, LocalEndSessionResponse)> response_callback)
{
  auto &request_cpy = *request;
  enforcer_->get_event_base().runInEventBaseThread(
    [this, request_cpy, response_callback]() {
      try {
        auto reporter = reporter_;
        enforcer_->terminate_subscriber(
          request_cpy.id(), [reporter](SessionTerminateRequest term_req) {
            // report to cloud
            report_termination(*reporter, term_req);
          });
        response_callback(grpc::Status::OK, LocalEndSessionResponse());
      } catch (const SessionNotFound &ex) {
        MLOG(MERROR) << "Failed to find session to terminate for subscriber "
                     << request_cpy.id();
        Status status(grpc::FAILED_PRECONDITION, "Session not found");
        response_callback(status, LocalEndSessionResponse());
      }
    });
}

} // namespace magma
