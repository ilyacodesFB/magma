/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The OpenAirInterface Software Alliance licenses this file to You under
 * the Apache License, Version 2.0  (the "License"); you may not use this file
 * except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *-------------------------------------------------------------------------------
 * For more information about the OpenAirInterface (OAI) Software Alliance:
 *      contact@openairinterface.org
 */

/*! \file mme_app_location.c
   \brief
   \author Sebastien ROUX, Lionel GAUTHIER
   \version 1.0
   \company Eurecom
   \email: lionel.gauthier@eurecom.fr
*/

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>

#include "bstrlib.h"
#include "log.h"
#include "assertions.h"
#include "common_types.h"
#include "conversions.h"
#include "intertask_interface.h"
#include "common_defs.h"
#include "mme_config.h"
#include "mme_app_ue_context.h"
#include "mme_app_defs.h"
#include "timer.h"
#include "3gpp_23.003.h"
#include "3gpp_36.401.h"
#include "TrackingAreaIdentity.h"
#include "emm_data.h"
#include "intertask_interface_types.h"
#include "itti_types.h"
#include "mme_app_desc.h"
#include "nas_messages_types.h"
#include "s6a_messages_types.h"
#include "service303.h"
#include "sgs_messages_types.h"
#include "esm_proc.h"

//------------------------------------------------------------------------------
int mme_app_send_s6a_update_location_req(
  struct ue_mm_context_s *const ue_context_p)
{
  OAILOG_FUNC_IN(LOG_MME_APP);
  MessageDef *message_p = NULL;
  s6a_update_location_req_t *s6a_ulr_p = NULL;
  int rc = RETURNok;

  OAILOG_INFO(
    TASK_MME_APP, "Sending S6A UPDATE LOCATION REQ to S6A, ue_id = %u\n",
    ue_context_p->mme_ue_s1ap_id);
  message_p = itti_alloc_new_message(TASK_MME_APP, S6A_UPDATE_LOCATION_REQ);

  if (message_p == NULL) {
    OAILOG_FUNC_RETURN(LOG_MME_APP, RETURNerror);
  }

  s6a_ulr_p = &message_p->ittiMsg.s6a_update_location_req;
  memset((void *) s6a_ulr_p, 0, sizeof(s6a_update_location_req_t));
  IMSI64_TO_STRING(
    (ue_context_p->emm_context._imsi64),
    s6a_ulr_p->imsi,
    ue_context_p->emm_context._imsi.length);
  s6a_ulr_p->imsi_length = strlen(s6a_ulr_p->imsi);
  s6a_ulr_p->initial_attach = INITIAL_ATTACH;
  //memcpy(&s6a_ulr_p->visited_plmn, &ue_context_p->e_utran_cgi.plmn, sizeof
  //(plmn_t));
  plmn_t visited_plmn = {0};
  visited_plmn.mcc_digit1 =
    ue_context_p->emm_context.originating_tai.mcc_digit1;
  visited_plmn.mcc_digit2 =
    ue_context_p->emm_context.originating_tai.mcc_digit2;
  visited_plmn.mcc_digit3 =
    ue_context_p->emm_context.originating_tai.mcc_digit3;
  visited_plmn.mnc_digit1 =
    ue_context_p->emm_context.originating_tai.mnc_digit1;
  visited_plmn.mnc_digit2 =
    ue_context_p->emm_context.originating_tai.mnc_digit2;
  visited_plmn.mnc_digit3 =
    ue_context_p->emm_context.originating_tai.mnc_digit3;

  memcpy(&s6a_ulr_p->visited_plmn, &visited_plmn, sizeof(plmn_t));
  s6a_ulr_p->rat_type = RAT_EUTRAN;
  OAILOG_DEBUG(
    TASK_MME_APP, "S6A ULR: RAT TYPE = (%d) for (ue_id = %u)\n",
    s6a_ulr_p->rat_type,
    ue_context_p->mme_ue_s1ap_id);
  /*
   * Check if we already have UE data
   * set the skip subscriber data flas as true in case we are sending ULR against recieved HSS Reset
   */
  if (ue_context_p->location_info_confirmed_in_hss == true) {
    s6a_ulr_p->skip_subscriber_data = 1;
    OAILOG_DEBUG(
      TASK_MME_APP, "S6A Location information confirmed in HSS (%d) for (ue_id = %u)\n",
      ue_context_p->location_info_confirmed_in_hss,
      ue_context_p->mme_ue_s1ap_id);
  } else {
    s6a_ulr_p->skip_subscriber_data = 0;
    OAILOG_DEBUG(
      TASK_MME_APP, "S6A Location information not confirmed in HSS (%d) for (ue_id = %u)\n",
      ue_context_p->location_info_confirmed_in_hss,
      ue_context_p->mme_ue_s1ap_id);
  }

  //Check if we have voice domain preference IE and send to S6a task
  if(ue_context_p->emm_context.volte_params.presencemask & VOICE_DOMAIN_PREF_UE_USAGE_SETTING) {
    s6a_ulr_p->voice_dom_pref_ue_usg_setting =
      ue_context_p->emm_context.volte_params.voice_domain_preference_and_ue_usage_setting;
    s6a_ulr_p->presencemask |= S6A_PDN_CONFIG_VOICE_DOM_PREF;
  }
  OAILOG_DEBUG(
    LOG_MME_APP,
    "0 S6A_UPDATE_LOCATION_REQ imsi %s with length %d for (ue_id = %u)\n",
    s6a_ulr_p->imsi,
    s6a_ulr_p->imsi_length,
    ue_context_p->mme_ue_s1ap_id);
  rc = itti_send_msg_to_task(TASK_S6A, INSTANCE_DEFAULT, message_p);
  /*
   * Do not start this timer in case we are sending ULR after receiving HSS reset
   */
  if (ue_context_p->location_info_confirmed_in_hss == false) {
    // Start ULR Response timer
    if (
      timer_setup(
        ue_context_p->ulr_response_timer.sec,
        0,
        TASK_MME_APP,
        INSTANCE_DEFAULT,
        TIMER_ONE_SHOT,
        (void *) &(ue_context_p->mme_ue_s1ap_id),
        sizeof(mme_ue_s1ap_id_t),
        &(ue_context_p->ulr_response_timer.id)) < 0) {
      OAILOG_ERROR(
        LOG_MME_APP,
        "Failed to start Update location update response timer for UE id  %d "
        "\n",
        ue_context_p->mme_ue_s1ap_id);
      ue_context_p->ulr_response_timer.id = MME_APP_TIMER_INACTIVE_ID;
    } else {
      OAILOG_DEBUG(
        LOG_MME_APP,
        "Started location update response timer for UE id  %d \n",
        ue_context_p->mme_ue_s1ap_id);
    }
  }
  OAILOG_FUNC_RETURN(LOG_MME_APP, rc);
}

int _handle_ula_failure(struct ue_mm_context_s *ue_context_p)
{
  MessageDef *message_p = NULL;
  int rc = RETURNok;

  OAILOG_FUNC_IN(LOG_MME_APP);

  // Stop ULR Response timer if running
  if (ue_context_p->ulr_response_timer.id != MME_APP_TIMER_INACTIVE_ID) {
    if (timer_remove(ue_context_p->ulr_response_timer.id, NULL)) {
      OAILOG_ERROR(
        LOG_MME_APP,
        "Failed to stop Update location update response timer for UE id  %d \n",
        ue_context_p->mme_ue_s1ap_id);
    }
    ue_context_p->ulr_response_timer.id = MME_APP_TIMER_INACTIVE_ID;
  }
  // Send PDN CONNECTIVITY FAIL message  to NAS layer
  increment_counter("mme_s6a_update_location_ans", 1, 1, "result", "failure");
  message_p = itti_alloc_new_message(TASK_MME_APP, NAS_PDN_CONNECTIVITY_FAIL);
  itti_nas_pdn_connectivity_fail_t *nas_pdn_connectivity_fail =
    &message_p->ittiMsg.nas_pdn_connectivity_fail;
  memset(
    (void *) nas_pdn_connectivity_fail,
    0,
    sizeof(itti_nas_pdn_connectivity_fail_t));
  if(ue_context_p->emm_context.esm_ctx.esm_proc_data) {
    nas_pdn_connectivity_fail->pti = ue_context_p->emm_context.esm_ctx.
      esm_proc_data->pti;
  } else {
      OAILOG_ERROR(
        LOG_MME_APP," esm_proc_data is NULL, so failed to fetch pti \n");
  }
  nas_pdn_connectivity_fail->ue_id = ue_context_p->mme_ue_s1ap_id;
  nas_pdn_connectivity_fail->cause = CAUSE_SYSTEM_FAILURE;
  rc = itti_send_msg_to_task(TASK_NAS_MME, INSTANCE_DEFAULT, message_p);
  OAILOG_FUNC_RETURN(LOG_MME_APP, rc);
}

//------------------------------------------------------------------------------
int mme_app_handle_s6a_update_location_ans(
  const s6a_update_location_ans_t *const ula_pP)
{
  OAILOG_FUNC_IN(LOG_MME_APP);
  uint64_t imsi64 = 0;
  struct ue_mm_context_s *ue_mm_context = NULL;
  int rc = RETURNok;

  DevAssert(ula_pP);

  IMSI_STRING_TO_IMSI64((char *) ula_pP->imsi, &imsi64);
  OAILOG_DEBUG(
    LOG_MME_APP, "Handling imsi " IMSI_64_FMT "\n", imsi64);

  if (
    (ue_mm_context = mme_ue_context_exists_imsi(
       &mme_app_desc.mme_ue_contexts, imsi64)) == NULL) {
    OAILOG_ERROR(
      LOG_MME_APP, "That's embarrassing as we don't know this IMSI\n");
    OAILOG_FUNC_RETURN(LOG_MME_APP, RETURNerror);
  }
  if (ula_pP->result.present == S6A_RESULT_BASE) {
    if (ula_pP->result.choice.base != DIAMETER_SUCCESS) {
      /*
       * The update location procedure has failed. Notify the NAS layer
       * and don't initiate the bearer creation on S-GW side.
       */
      OAILOG_ERROR(
        LOG_MME_APP,
        "ULR/ULA procedure returned non success (ULA.result.choice.base=%d)\n",
        ula_pP->result.choice.base);
      if (_handle_ula_failure(ue_mm_context) == RETURNok) {
        OAILOG_DEBUG(LOG_MME_APP, "Sent PDN Connectivity failure to NAS for ue_id (%u)\n",
        ue_mm_context->mme_ue_s1ap_id);
        unlock_ue_contexts(ue_mm_context);
        OAILOG_FUNC_RETURN(LOG_MME_APP, rc);
      } else {
        OAILOG_ERROR(
          LOG_MME_APP, "Failed to send PDN Connectivity failure to NAS for ue_id (%u)\n",
          ue_mm_context->mme_ue_s1ap_id);
        unlock_ue_contexts(ue_mm_context);
        OAILOG_FUNC_RETURN(LOG_MME_APP, RETURNerror);
      }
    }
  } else {
    /*
     * The update location procedure has failed. Notify the NAS layer
     * and don't initiate the bearer creation on S-GW side.
     */
    OAILOG_ERROR(
      LOG_MME_APP,
      "ULR/ULA procedure returned non success (ULA.result.present=%d)\n",
      ula_pP->result.present);
    if (_handle_ula_failure(ue_mm_context) == RETURNok) {
      OAILOG_DEBUG(LOG_MME_APP, "Sent PDN Connectivity failure to NAS for ue_id (%u)\n",
      ue_mm_context->mme_ue_s1ap_id);
      unlock_ue_contexts(ue_mm_context);
      OAILOG_FUNC_RETURN(LOG_MME_APP, rc);
    } else {
      OAILOG_ERROR(
        LOG_MME_APP, "Failed to send PDN Connectivity failure to NAS for ue_id (%u)\n",
        ue_mm_context->mme_ue_s1ap_id);
      unlock_ue_contexts(ue_mm_context);
      OAILOG_FUNC_RETURN(LOG_MME_APP, RETURNerror);
    }
  }

  // Stop ULR Response timer if running
  if (ue_mm_context->ulr_response_timer.id != MME_APP_TIMER_INACTIVE_ID) {
    if (timer_remove(ue_mm_context->ulr_response_timer.id, NULL)) {
      OAILOG_ERROR(
        LOG_MME_APP,
        "Failed to stop Update location update response timer for UE id  %d \n",
        ue_mm_context->mme_ue_s1ap_id);
    }
    ue_mm_context->ulr_response_timer.id = MME_APP_TIMER_INACTIVE_ID;
  }

  ue_mm_context->subscription_known = SUBSCRIPTION_KNOWN;
  ue_mm_context->sub_status = ula_pP->subscription_data.subscriber_status;
  ue_mm_context->access_restriction_data =
    ula_pP->subscription_data.access_restriction;
  /*
   * Copy the subscribed ambr to the sgw create session request message
   */
  memcpy(
    &ue_mm_context->subscribed_ue_ambr,
    &ula_pP->subscription_data.subscribed_ambr,
    sizeof(ambr_t));
  OAILOG_DEBUG(
    LOG_MME_APP,
    "Received UL rate %" PRIu64 " and DL rate %" PRIu64 "\n",
    ue_mm_context->subscribed_ue_ambr.br_ul,
    ue_mm_context->subscribed_ue_ambr.br_dl);

  if (ula_pP->subscription_data.msisdn_length != 0) {
    ue_mm_context->msisdn = blk2bstr(
      ula_pP->subscription_data.msisdn,
      ula_pP->subscription_data.msisdn_length);
  } else {
    OAILOG_ERROR(
      LOG_MME_APP,
      "No MSISDN received for %s " IMSI_64_FMT "\n",
      __FUNCTION__,
      imsi64);
  }
  ue_mm_context->rau_tau_timer = ula_pP->subscription_data.rau_tau_timer;
  ue_mm_context->network_access_mode = ula_pP->subscription_data.access_mode;
  memcpy(
    &ue_mm_context->apn_config_profile,
    &ula_pP->subscription_data.apn_config_profile,
    sizeof(apn_config_profile_t));

  MessageDef *message_p = NULL;
  itti_nas_pdn_config_rsp_t *nas_pdn_config_rsp = NULL;

  message_p = itti_alloc_new_message(TASK_MME_APP, NAS_PDN_CONFIG_RSP);

  if (message_p == NULL) {
    OAILOG_ERROR(
      LOG_MME_APP,
      "Message pointer is NULL while allocating new message for PDN Config Rsp, (ue_id = %u)\n",
      ue_mm_context->mme_ue_s1ap_id);
    unlock_ue_contexts(ue_mm_context);
    OAILOG_FUNC_RETURN(LOG_MME_APP, RETURNerror);
  }

  /*
   * Set the value of  Mobile Reachability timer based on value of T3412 (Periodic TAU timer) sent in Attach accept /TAU accept.
   * Set it to MME_APP_DELTA_T3412_REACHABILITY_TIMER minutes greater than T3412.
   * Set the value of Implicit timer. Set it to MME_APP_DELTA_REACHABILITY_IMPLICIT_DETACH_TIMER minutes greater than  Mobile Reachability timer
  */
  ue_mm_context->mobile_reachability_timer.id = MME_APP_TIMER_INACTIVE_ID;
  ue_mm_context->mobile_reachability_timer.sec =
    ((mme_config.nas_config.t3412_min) +
     MME_APP_DELTA_T3412_REACHABILITY_TIMER) *
    60;
  ue_mm_context->implicit_detach_timer.id = MME_APP_TIMER_INACTIVE_ID;
  ue_mm_context->implicit_detach_timer.sec =
    (ue_mm_context->mobile_reachability_timer.sec) +
    MME_APP_DELTA_REACHABILITY_IMPLICIT_DETACH_TIMER * 60;

  /*
   * Set the flag: send_ue_purge_request to indicate that
   * Update Location procedure is completed.
   * During UE initiated detach/Implicit detach this MME would send PUR to hss,
   * if this flag is true.
  */
  ue_mm_context->send_ue_purge_request = true;
  /*
   * Set the flag: location_info_confirmed_in_hss to false to indicate that
   * Update Location procedure is completed.
   * During HSS Reset
   * if this flag is true.
  */
  if (ue_mm_context->location_info_confirmed_in_hss == true) {
    ue_mm_context->location_info_confirmed_in_hss = false;
  }

  nas_pdn_config_rsp = &message_p->ittiMsg.nas_pdn_config_rsp;
  nas_pdn_config_rsp->ue_id = ue_mm_context->mme_ue_s1ap_id;
  OAILOG_INFO(LOG_MME_APP, "Sending PDN CONFIG RSP to NAS for (ue_id = %u)\n",
    nas_pdn_config_rsp->ue_id);
  rc = itti_send_msg_to_task(TASK_NAS_MME, INSTANCE_DEFAULT, message_p);

  unlock_ue_contexts(ue_mm_context);
  OAILOG_FUNC_RETURN(LOG_MME_APP, rc);
}

int mme_app_handle_s6a_cancel_location_req(
  const s6a_cancel_location_req_t *const clr_pP)
{
  int rc = RETURNok;
  uint64_t imsi = 0;
  struct ue_mm_context_s *ue_context_p = NULL;
  int cla_result = DIAMETER_SUCCESS;
  itti_nas_sgs_detach_req_t sgs_detach_req = {0};

  OAILOG_FUNC_IN(LOG_MME_APP);
  DevAssert(clr_pP);

  IMSI_STRING_TO_IMSI64((char *) clr_pP->imsi, &imsi);
  OAILOG_DEBUG(
    LOG_MME_APP,
    "S6a Cancel Location Request for imsi " IMSI_64_FMT "\n",
    imsi);

  if (
    (mme_app_send_s6a_cancel_location_ans(
      cla_result, clr_pP->imsi, clr_pP->imsi_length, clr_pP->msg_cla_p)) !=
    RETURNok) {
    OAILOG_ERROR(
      LOG_MME_APP,
      "S6a Cancel Location Request: Failed to send Cancel Location Answer from "
      "MME app for imsi " IMSI_64_FMT "\n",
      imsi);
  }

  if (
    (ue_context_p = mme_ue_context_exists_imsi(
       &mme_app_desc.mme_ue_contexts, imsi)) == NULL) {
    OAILOG_ERROR(LOG_MME_APP,
      "IMSI is not present in the MME context for imsi " IMSI_64_FMT "\n",
      imsi);
    OAILOG_FUNC_RETURN(LOG_MME_APP, RETURNerror);
  }
  if (clr_pP->cancellation_type != SUBSCRIPTION_WITHDRAWL) {
    OAILOG_ERROR(
      LOG_MME_APP,
      "S6a Cancel Location Request: Cancellation_type not supported %d for"
      "imsi " IMSI_64_FMT "\n",
      clr_pP->cancellation_type,
      imsi);
    unlock_ue_contexts(ue_context_p);
    OAILOG_FUNC_RETURN(LOG_MME_APP, RETURNerror);
  }
  /*
   * set the flag: hss_initiated_detach to indicate that,
   * hss has initiated detach and MME shall not send PUR to hss
   */
  ue_context_p->hss_initiated_detach = true;

  /*
   * Check UE's S1 connection status.If UE is in connected state,
   * send Detach Request to UE. If UE is in idle state,
   * Page UE to bring it back to connected mode and then send Detach Request
   */
  if (ue_context_p->ecm_state == ECM_IDLE) {
    /* Page the UE to bring it back to connected mode
     * and then send Detach Request
    */
    mme_app_paging_request_helper(
      ue_context_p, true, false /* s-tmsi */, CN_DOMAIN_PS);
    // Set the flag and send detach to UE after receiving service req
    ue_context_p->emm_context.nw_init_bearer_deactv = true;
    OAILOG_FUNC_RETURN(LOG_MME_APP, RETURNok);

  } else {
    // Send N/W Initiated Detach Request to NAS

    OAILOG_INFO(LOG_MME_APP, "Sending Detach to NAS for (ue_id = %u)\n",
      ue_context_p->mme_ue_s1ap_id);
    rc = mme_app_send_nas_detach_request(
      ue_context_p->mme_ue_s1ap_id, HSS_INITIATED_EPS_DETACH);

    // Send SGS explicit network initiated Detach Ind to SGS
    if (ue_context_p->sgs_context) {
      sgs_detach_req.ue_id = ue_context_p->mme_ue_s1ap_id;
      sgs_detach_req.detach_type = SGS_DETACH_TYPE_NW_INITIATED_EPS;
      mme_app_handle_sgs_detach_req(&sgs_detach_req);
    }
  }
  unlock_ue_contexts(ue_context_p);
  OAILOG_FUNC_RETURN(LOG_MME_APP, rc);
}

int mme_app_send_s6a_cancel_location_ans(
  int cla_result,
  const char *imsi,
  uint8_t imsi_length,
  void *msg_cla_p)
{
  MessageDef *message_p = NULL;
  s6a_cancel_location_ans_t *s6a_cla_p = NULL;
  int rc = RETURNok;

  OAILOG_FUNC_IN(LOG_MME_APP);

  message_p = itti_alloc_new_message(TASK_MME_APP, S6A_CANCEL_LOCATION_ANS);

  if (message_p == NULL) {
    OAILOG_FUNC_RETURN(LOG_MME_APP, RETURNerror);
  }

  s6a_cla_p = &message_p->ittiMsg.s6a_cancel_location_ans;
  memset((void *) s6a_cla_p, 0, sizeof(s6a_cancel_location_ans_t));

  /* Using the IMSI details deom CLR */
  memcpy(s6a_cla_p->imsi, imsi, imsi_length);
  s6a_cla_p->imsi_length = imsi_length;

  s6a_cla_p->result = cla_result;
  s6a_cla_p->msg_cla_p = msg_cla_p;
  rc = itti_send_msg_to_task(TASK_S6A, INSTANCE_DEFAULT, message_p);
  OAILOG_FUNC_RETURN(LOG_MME_APP, rc);
}
