/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation. All rights reserved.
 *   Copyright (c) 2019 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2021 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/stdinc.h"

#include "nvmf_internal.h"

#include "spdk/bdev.h"
#include "spdk/endian.h"
#include "spdk/thread.h"
#include "spdk/likely.h"
#include "spdk/nvme.h"
#include "spdk/nvmf_cmd.h"
#include "spdk/nvmf_spec.h"
#include "spdk/trace.h"
#include "spdk/scsi_spec.h"
#include "spdk/string.h"
#include "spdk/util.h"

#include "spdk/log.h"

static bool
nvmf_subsystem_bdev_io_type_supported(struct spdk_nvmf_subsystem *subsystem,
				      enum spdk_bdev_io_type io_type)
{
	struct spdk_nvmf_ns *ns;

	for (ns = spdk_nvmf_subsystem_get_first_ns(subsystem); ns != NULL;
	     ns = spdk_nvmf_subsystem_get_next_ns(subsystem, ns)) {
		if (ns->bdev == NULL) {
			continue;
		}

		if (!spdk_bdev_io_type_supported(ns->bdev, io_type)) {
			SPDK_DEBUGLOG(nvmf,
				      "Subsystem %s namespace %u (%s) does not support io_type %d\n",
				      spdk_nvmf_subsystem_get_nqn(subsystem),
				      ns->opts.nsid, spdk_bdev_get_name(ns->bdev), (int)io_type);
			return false;
		}
	}

	SPDK_DEBUGLOG(nvmf, "All devices in Subsystem %s support io_type %d\n",
		      spdk_nvmf_subsystem_get_nqn(subsystem), (int)io_type);
	return true;
}

bool
nvmf_ctrlr_dsm_supported(struct spdk_nvmf_ctrlr *ctrlr)
{
	return nvmf_subsystem_bdev_io_type_supported(ctrlr->subsys, SPDK_BDEV_IO_TYPE_UNMAP);
}

bool
nvmf_ctrlr_write_zeroes_supported(struct spdk_nvmf_ctrlr *ctrlr)
{
	return nvmf_subsystem_bdev_io_type_supported(ctrlr->subsys, SPDK_BDEV_IO_TYPE_WRITE_ZEROES);
}

bool
nvmf_ctrlr_copy_supported(struct spdk_nvmf_ctrlr *ctrlr)
{
	return nvmf_subsystem_bdev_io_type_supported(ctrlr->subsys, SPDK_BDEV_IO_TYPE_COPY);
}

static void
nvmf_bdev_ctrlr_complete_cmd(struct spdk_bdev_io *bdev_io, bool success,
			     void *cb_arg)
{
	struct spdk_nvmf_request	*req = cb_arg;
	struct spdk_nvme_cpl		*response = &req->rsp->nvme_cpl;
	int				first_sc = 0, first_sct = 0, sc = 0, sct = 0;
	uint32_t			cdw0 = 0;
	struct spdk_nvmf_request	*first_req = req->first_fused_req;

	if (spdk_unlikely(first_req != NULL)) {
		/* fused commands - get status for both operations */
		struct spdk_nvme_cpl *first_response = &first_req->rsp->nvme_cpl;

		spdk_bdev_io_get_nvme_fused_status(bdev_io, &cdw0, &first_sct, &first_sc, &sct, &sc);
		first_response->cdw0 = cdw0;
		first_response->status.sc = first_sc;
		first_response->status.sct = first_sct;

		/* first request should be completed */
		spdk_nvmf_request_complete(first_req);
		req->first_fused_req = NULL;
	} else {
		spdk_bdev_io_get_nvme_status(bdev_io, &cdw0, &sct, &sc);
	}

	response->cdw0 = cdw0;
	response->status.sc = sc;
	response->status.sct = sct;

	spdk_nvmf_request_complete(req);
	spdk_bdev_free_io(bdev_io);
}

static void
nvmf_bdev_ctrlr_complete_admin_cmd(struct spdk_bdev_io *bdev_io, bool success,
				   void *cb_arg)
{
	struct spdk_nvmf_request *req = cb_arg;

	if (req->cmd_cb_fn) {
		req->cmd_cb_fn(req);
	}

	nvmf_bdev_ctrlr_complete_cmd(bdev_io, success, req);
}

void
nvmf_bdev_ctrlr_identify_ns(struct spdk_nvmf_ns *ns, struct spdk_nvme_ns_data *nsdata,
			    bool dif_insert_or_strip)
{
	struct spdk_bdev *bdev = ns->bdev;
	uint64_t num_blocks;
	uint32_t phys_blocklen;
	uint32_t max_copy;

	num_blocks = spdk_bdev_get_num_blocks(bdev);

	nsdata->nsze = num_blocks;
	nsdata->ncap = num_blocks;
	nsdata->nuse = num_blocks;
	nsdata->nlbaf = 0;
	nsdata->flbas.format = 0;
	nsdata->flbas.msb_format = 0;
	nsdata->nacwu = spdk_bdev_get_acwu(bdev) - 1; /* nacwu is 0-based */
	if (!dif_insert_or_strip) {
		nsdata->lbaf[0].ms = spdk_bdev_get_md_size(bdev);
		nsdata->lbaf[0].lbads = spdk_u32log2(spdk_bdev_get_block_size(bdev));
		if (nsdata->lbaf[0].ms != 0) {
			nsdata->flbas.extended = 1;
			nsdata->mc.extended = 1;
			nsdata->mc.pointer = 0;
			nsdata->dps.md_start = spdk_bdev_is_dif_head_of_md(bdev);
			/* NVMf library doesn't process PRACT and PRCHK flags, we
			 * leave the use of extended LBA buffer to users.
			 */
			nsdata->dps.pit = SPDK_NVME_FMT_NVM_PROTECTION_DISABLE;
		}
	} else {
		nsdata->lbaf[0].ms = 0;
		nsdata->lbaf[0].lbads = spdk_u32log2(spdk_bdev_get_data_block_size(bdev));
	}

	phys_blocklen = spdk_bdev_get_physical_block_size(bdev);
	assert(phys_blocklen > 0);
	/* Linux driver uses min(nawupf, npwg) to set physical_block_size */
	nsdata->nsfeat.optperf = 1;
	nsdata->nsfeat.ns_atomic_write_unit = 1;
	nsdata->npwg = (phys_blocklen >> nsdata->lbaf[0].lbads) - 1;
	nsdata->nawupf = nsdata->npwg;
	nsdata->npwa = nsdata->npwg;
	nsdata->npdg = nsdata->npwg;
	nsdata->npda = nsdata->npwg;

	if (spdk_bdev_get_write_unit_size(bdev) == 1) {
		nsdata->noiob = spdk_bdev_get_optimal_io_boundary(bdev);
	}
	nsdata->nmic.can_share = 1;
	if (ns->ptpl_file != NULL) {
		nsdata->nsrescap.rescap.persist = 1;
	}
	nsdata->nsrescap.rescap.write_exclusive = 1;
	nsdata->nsrescap.rescap.exclusive_access = 1;
	nsdata->nsrescap.rescap.write_exclusive_reg_only = 1;
	nsdata->nsrescap.rescap.exclusive_access_reg_only = 1;
	nsdata->nsrescap.rescap.write_exclusive_all_reg = 1;
	nsdata->nsrescap.rescap.exclusive_access_all_reg = 1;
	nsdata->nsrescap.rescap.ignore_existing_key = 1;

	SPDK_STATIC_ASSERT(sizeof(nsdata->nguid) == sizeof(ns->opts.nguid), "size mismatch");
	memcpy(nsdata->nguid, ns->opts.nguid, sizeof(nsdata->nguid));

	SPDK_STATIC_ASSERT(sizeof(nsdata->eui64) == sizeof(ns->opts.eui64), "size mismatch");
	memcpy(&nsdata->eui64, ns->opts.eui64, sizeof(nsdata->eui64));

	/* For now we support just one source range for copy command */
	nsdata->msrc = 0;

	max_copy = spdk_bdev_get_max_copy(bdev);
	if (max_copy == 0 || max_copy > UINT16_MAX) {
		/* Zero means copy size is unlimited */
		nsdata->mcl = UINT16_MAX;
		nsdata->mssrl = UINT16_MAX;
	} else {
		nsdata->mcl = max_copy;
		nsdata->mssrl = max_copy;
	}
}

void
nvmf_bdev_ctrlr_identify_ns_kv(struct spdk_nvmf_ns *ns, struct spdk_nvme_kv_ns_data *nsdata)
{
	struct spdk_bdev *bdev = ns->bdev;

	nsdata->nsze = spdk_bdev_get_num_blocks(bdev);
	nsdata->nuse = spdk_bdev_get_num_blocks(bdev);
	nsdata->nkvf = 1;
	nsdata->kvf[0].kv_value_max_len = KV_MAX_VALUE_SIZE;
	nsdata->kvf[0].kv_key_max_len = KV_MAX_KEY_SIZE;
	nsdata->kvf[0].kv_max_num_keys = 0; /* Unlimited */
	nsdata->nmic.can_share = 1;
	if (ns->ptpl_file != NULL) {
		nsdata->nsrescap.rescap.persist = 1;
	}
	nsdata->nsrescap.rescap.write_exclusive = 1;
	nsdata->nsrescap.rescap.exclusive_access = 1;
	nsdata->nsrescap.rescap.write_exclusive_reg_only = 1;
	nsdata->nsrescap.rescap.exclusive_access_reg_only = 1;
	nsdata->nsrescap.rescap.write_exclusive_all_reg = 1;
	nsdata->nsrescap.rescap.exclusive_access_all_reg = 1;
	nsdata->nsrescap.rescap.ignore_existing_key = 1;

	SPDK_STATIC_ASSERT(sizeof(nsdata->nguid) == sizeof(ns->opts.nguid), "size mismatch");
	memcpy(nsdata->nguid, ns->opts.nguid, sizeof(nsdata->nguid));

	SPDK_STATIC_ASSERT(sizeof(nsdata->eui64) == sizeof(ns->opts.eui64), "size mismatch");
	memcpy(&nsdata->eui64, ns->opts.eui64, sizeof(nsdata->eui64));
}

static void
nvmf_bdev_ctrlr_get_rw_params(const struct spdk_nvme_cmd *cmd, uint64_t *start_lba,
			      uint64_t *num_blocks)
{
	/* SLBA: CDW10 and CDW11 */
	*start_lba = from_le64(&cmd->cdw10);

	/* NLB: CDW12 bits 15:00, 0's based */
	*num_blocks = (from_le32(&cmd->cdw12) & 0xFFFFu) + 1;
}

static bool
nvmf_bdev_ctrlr_lba_in_range(uint64_t bdev_num_blocks, uint64_t io_start_lba,
			     uint64_t io_num_blocks)
{
	if (io_start_lba + io_num_blocks > bdev_num_blocks ||
	    io_start_lba + io_num_blocks < io_start_lba) {
		return false;
	}

	return true;
}

static void
nvmf_ctrlr_process_io_cmd_resubmit(void *arg)
{
	struct spdk_nvmf_request *req = arg;
	int rc;

	rc = nvmf_ctrlr_process_io_cmd(req);
	if (rc == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE) {
		spdk_nvmf_request_complete(req);
	}
}

static void
nvmf_ctrlr_process_admin_cmd_resubmit(void *arg)
{
	struct spdk_nvmf_request *req = arg;
	int rc;

	rc = nvmf_ctrlr_process_admin_cmd(req);
	if (rc == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE) {
		spdk_nvmf_request_complete(req);
	}
}

static void
nvmf_bdev_ctrl_queue_io(struct spdk_nvmf_request *req, struct spdk_bdev *bdev,
			struct spdk_io_channel *ch, spdk_bdev_io_wait_cb cb_fn, void *cb_arg)
{
	int rc;

	req->bdev_io_wait.bdev = bdev;
	req->bdev_io_wait.cb_fn = cb_fn;
	req->bdev_io_wait.cb_arg = cb_arg;

	rc = spdk_bdev_queue_io_wait(bdev, ch, &req->bdev_io_wait);
	if (rc != 0) {
		assert(false);
	}
	req->qpair->group->stat.pending_bdev_io++;
}

bool
nvmf_bdev_zcopy_enabled(struct spdk_bdev *bdev)
{
	return spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_ZCOPY);
}

int
nvmf_bdev_ctrlr_read_cmd(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
			 struct spdk_io_channel *ch, struct spdk_nvmf_request *req)
{
	uint64_t bdev_num_blocks = spdk_bdev_get_num_blocks(bdev);
	uint32_t block_size = spdk_bdev_get_block_size(bdev);
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;
	uint64_t start_lba;
	uint64_t num_blocks;
	int rc;

	nvmf_bdev_ctrlr_get_rw_params(cmd, &start_lba, &num_blocks);

	if (spdk_unlikely(!nvmf_bdev_ctrlr_lba_in_range(bdev_num_blocks, start_lba, num_blocks))) {
		SPDK_ERRLOG("end of media\n");
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_LBA_OUT_OF_RANGE;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (spdk_unlikely(num_blocks * block_size > req->length)) {
		SPDK_ERRLOG("Read NLB %" PRIu64 " * block size %" PRIu32 " > SGL length %" PRIu32 "\n",
			    num_blocks, block_size, req->length);
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	assert(!spdk_nvmf_request_using_zcopy(req));

	rc = spdk_bdev_readv_blocks(desc, ch, req->iov, req->iovcnt, start_lba, num_blocks,
				    nvmf_bdev_ctrlr_complete_cmd, req);
	if (spdk_unlikely(rc)) {
		if (rc == -ENOMEM) {
			nvmf_bdev_ctrl_queue_io(req, bdev, ch, nvmf_ctrlr_process_io_cmd_resubmit, req);
			return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
		}
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
}

int
nvmf_bdev_ctrlr_write_cmd(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
			  struct spdk_io_channel *ch, struct spdk_nvmf_request *req)
{
	uint64_t bdev_num_blocks = spdk_bdev_get_num_blocks(bdev);
	uint32_t block_size = spdk_bdev_get_block_size(bdev);
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;
	uint64_t start_lba;
	uint64_t num_blocks;
	int rc;

	nvmf_bdev_ctrlr_get_rw_params(cmd, &start_lba, &num_blocks);

	if (spdk_unlikely(!nvmf_bdev_ctrlr_lba_in_range(bdev_num_blocks, start_lba, num_blocks))) {
		SPDK_ERRLOG("end of media\n");
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_LBA_OUT_OF_RANGE;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (spdk_unlikely(num_blocks * block_size > req->length)) {
		SPDK_ERRLOG("Write NLB %" PRIu64 " * block size %" PRIu32 " > SGL length %" PRIu32 "\n",
			    num_blocks, block_size, req->length);
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	assert(!spdk_nvmf_request_using_zcopy(req));

	rc = spdk_bdev_writev_blocks(desc, ch, req->iov, req->iovcnt, start_lba, num_blocks,
				     nvmf_bdev_ctrlr_complete_cmd, req);
	if (spdk_unlikely(rc)) {
		if (rc == -ENOMEM) {
			nvmf_bdev_ctrl_queue_io(req, bdev, ch, nvmf_ctrlr_process_io_cmd_resubmit, req);
			return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
		}
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
}

int
nvmf_bdev_ctrlr_compare_cmd(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
			    struct spdk_io_channel *ch, struct spdk_nvmf_request *req)
{
	uint64_t bdev_num_blocks = spdk_bdev_get_num_blocks(bdev);
	uint32_t block_size = spdk_bdev_get_block_size(bdev);
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;
	uint64_t start_lba;
	uint64_t num_blocks;
	int rc;

	nvmf_bdev_ctrlr_get_rw_params(cmd, &start_lba, &num_blocks);

	if (spdk_unlikely(!nvmf_bdev_ctrlr_lba_in_range(bdev_num_blocks, start_lba, num_blocks))) {
		SPDK_ERRLOG("end of media\n");
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_LBA_OUT_OF_RANGE;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (spdk_unlikely(num_blocks * block_size > req->length)) {
		SPDK_ERRLOG("Compare NLB %" PRIu64 " * block size %" PRIu32 " > SGL length %" PRIu32 "\n",
			    num_blocks, block_size, req->length);
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	rc = spdk_bdev_comparev_blocks(desc, ch, req->iov, req->iovcnt, start_lba, num_blocks,
				       nvmf_bdev_ctrlr_complete_cmd, req);
	if (spdk_unlikely(rc)) {
		if (rc == -ENOMEM) {
			nvmf_bdev_ctrl_queue_io(req, bdev, ch, nvmf_ctrlr_process_io_cmd_resubmit, req);
			return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
		}
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
}

int
nvmf_bdev_ctrlr_compare_and_write_cmd(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
				      struct spdk_io_channel *ch, struct spdk_nvmf_request *cmp_req, struct spdk_nvmf_request *write_req)
{
	uint64_t bdev_num_blocks = spdk_bdev_get_num_blocks(bdev);
	uint32_t block_size = spdk_bdev_get_block_size(bdev);
	struct spdk_nvme_cmd *cmp_cmd = &cmp_req->cmd->nvme_cmd;
	struct spdk_nvme_cmd *write_cmd = &write_req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *rsp = &write_req->rsp->nvme_cpl;
	uint64_t write_start_lba, cmp_start_lba;
	uint64_t write_num_blocks, cmp_num_blocks;
	int rc;

	nvmf_bdev_ctrlr_get_rw_params(cmp_cmd, &cmp_start_lba, &cmp_num_blocks);
	nvmf_bdev_ctrlr_get_rw_params(write_cmd, &write_start_lba, &write_num_blocks);

	if (spdk_unlikely(write_start_lba != cmp_start_lba || write_num_blocks != cmp_num_blocks)) {
		SPDK_ERRLOG("Fused command start lba / num blocks mismatch\n");
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (spdk_unlikely(!nvmf_bdev_ctrlr_lba_in_range(bdev_num_blocks, write_start_lba,
			  write_num_blocks))) {
		SPDK_ERRLOG("end of media\n");
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_LBA_OUT_OF_RANGE;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (spdk_unlikely(write_num_blocks * block_size > write_req->length)) {
		SPDK_ERRLOG("Write NLB %" PRIu64 " * block size %" PRIu32 " > SGL length %" PRIu32 "\n",
			    write_num_blocks, block_size, write_req->length);
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	rc = spdk_bdev_comparev_and_writev_blocks(desc, ch, cmp_req->iov, cmp_req->iovcnt, write_req->iov,
			write_req->iovcnt, write_start_lba, write_num_blocks, nvmf_bdev_ctrlr_complete_cmd, write_req);
	if (spdk_unlikely(rc)) {
		if (rc == -ENOMEM) {
			nvmf_bdev_ctrl_queue_io(cmp_req, bdev, ch, nvmf_ctrlr_process_io_cmd_resubmit, cmp_req);
			nvmf_bdev_ctrl_queue_io(write_req, bdev, ch, nvmf_ctrlr_process_io_cmd_resubmit, write_req);
			return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
		}
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
}

int
nvmf_bdev_ctrlr_write_zeroes_cmd(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
				 struct spdk_io_channel *ch, struct spdk_nvmf_request *req)
{
	uint64_t bdev_num_blocks = spdk_bdev_get_num_blocks(bdev);
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;
	uint64_t start_lba;
	uint64_t num_blocks;
	int rc;

	nvmf_bdev_ctrlr_get_rw_params(cmd, &start_lba, &num_blocks);

	if (spdk_unlikely(!nvmf_bdev_ctrlr_lba_in_range(bdev_num_blocks, start_lba, num_blocks))) {
		SPDK_ERRLOG("end of media\n");
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_LBA_OUT_OF_RANGE;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	rc = spdk_bdev_write_zeroes_blocks(desc, ch, start_lba, num_blocks,
					   nvmf_bdev_ctrlr_complete_cmd, req);
	if (spdk_unlikely(rc)) {
		if (rc == -ENOMEM) {
			nvmf_bdev_ctrl_queue_io(req, bdev, ch, nvmf_ctrlr_process_io_cmd_resubmit, req);
			return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
		}
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
}

int
nvmf_bdev_ctrlr_flush_cmd(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
			  struct spdk_io_channel *ch, struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;
	int rc;

	/* As for NVMeoF controller, SPDK always set volatile write
	 * cache bit to 1, return success for those block devices
	 * which can't support FLUSH command.
	 */
	if (!spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_FLUSH)) {
		response->status.sct = SPDK_NVME_SCT_GENERIC;
		response->status.sc = SPDK_NVME_SC_SUCCESS;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	rc = spdk_bdev_flush_blocks(desc, ch, 0, spdk_bdev_get_num_blocks(bdev),
				    nvmf_bdev_ctrlr_complete_cmd, req);
	if (spdk_unlikely(rc)) {
		if (rc == -ENOMEM) {
			nvmf_bdev_ctrl_queue_io(req, bdev, ch, nvmf_ctrlr_process_io_cmd_resubmit, req);
			return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
		}
		response->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}
	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
}

int
nvmf_bdev_ctrlr_retrieve_cmd(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
			     struct spdk_io_channel *ch, struct spdk_nvmf_request *req)
{
	struct spdk_nvme_kv_cmd *cmd = (struct spdk_nvme_kv_cmd *)&req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;
	struct spdk_nvme_kv_key_t key;
	int rc;

	spdk_nvme_kv_cmd_get_key(cmd, &key);
	rc = spdk_bdev_kv_retrieve(desc, ch, key.kl, key.key, req->iov[0].iov_base,
				   req->iov[0].iov_len,
				   nvmf_bdev_ctrlr_complete_cmd, req);
	if (spdk_unlikely(rc)) {
		if (rc == -ENOMEM) {
			nvmf_bdev_ctrl_queue_io(req, bdev, ch, nvmf_ctrlr_process_io_cmd_resubmit, req);
			return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
		}
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
}

int
nvmf_bdev_ctrlr_store_cmd(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
			  struct spdk_io_channel *ch, struct spdk_nvmf_request *req)
{
	struct spdk_nvme_kv_cmd *cmd = (struct spdk_nvme_kv_cmd *)&req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;
	struct spdk_nvme_kv_key_t key;
	int rc;

	spdk_nvme_kv_cmd_get_key(cmd, &key);
	rc = spdk_bdev_kv_store(desc, ch, key.kl, key.key, req->iov[0].iov_base,
				req->iov[0].iov_len,
				nvmf_bdev_ctrlr_complete_cmd, req);
	if (spdk_unlikely(rc)) {
		if (rc == -ENOMEM) {
			nvmf_bdev_ctrl_queue_io(req, bdev, ch, nvmf_ctrlr_process_io_cmd_resubmit, req);
			return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
		}
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
}

static int
nvmf_bdev_kv_list_cb(struct spdk_io_channel *ch,
		     struct spdk_bdev_io *bdev_io, uint32_t key_len, const uint8_t *key, void *buffer,
		     uint32_t buffer_len, void **list_cb_arg)
{
	if (key_len > KV_MAX_KEY_SIZE) {
		SPDK_ERRLOG("Invalid key length %u\n", key_len);
		return -1;
	}
	struct spdk_nvme_kv_ns_list_data *list_data = (struct spdk_nvme_kv_ns_list_data *)buffer;
	struct spdk_nvme_kv_key_t *key_data = (struct spdk_nvme_kv_key_t *)*list_cb_arg;

	uint32_t bytes_consumed = (uint8_t *)key_data - (uint8_t *)list_data;
	uint32_t bytes_left = buffer_len - bytes_consumed;
	if (bytes_left >= sizeof(key_data->kl) + key_len) {
		key_data->kl = key_len;
		memcpy(key_data->key, key, key_len);
		list_data->nrk++;
		/* Roundup to the next 4 byte boundary */
		uint32_t new_key_len = ((sizeof(key_data->kl) + key_len + 3) / 4) * 4;
		key_data = (struct spdk_nvme_kv_key_t *)(((uint8_t *)key_data) + new_key_len);
		*list_cb_arg = key_data;
		return 1; /* Get next key */
	} else {
		return 0; /* No more room for keys */
	}
}

int
nvmf_bdev_ctrlr_list_cmd(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
			 struct spdk_io_channel *ch, struct spdk_nvmf_request *req)
{
	struct spdk_nvme_kv_cmd *cmd = (struct spdk_nvme_kv_cmd *)&req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;
	struct spdk_nvme_kv_key_t key;
	int rc;

	struct spdk_nvme_kv_ns_list_data *list_data = (struct spdk_nvme_kv_ns_list_data *)
			req->iov[0].iov_base;
	uint32_t bytes_left = req->iov[0].iov_len;
	if (bytes_left < sizeof(*list_data)) {
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}
	list_data->nrk = 0;
	spdk_nvme_kv_cmd_get_key(cmd, &key);
	rc = spdk_bdev_kv_list(desc, ch, key.kl, key.key, req->iov[0].iov_base,
			       req->iov[0].iov_len,
			       nvmf_bdev_ctrlr_complete_cmd, req, nvmf_bdev_kv_list_cb, &list_data->keys[0]);
	if (spdk_unlikely(rc)) {
		if (rc == -ENOMEM) {
			nvmf_bdev_ctrl_queue_io(req, bdev, ch, nvmf_ctrlr_process_io_cmd_resubmit, req->iov[0].iov_base);
			return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
		}
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
}

int
nvmf_bdev_ctrlr_exist_cmd(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
			  struct spdk_io_channel *ch, struct spdk_nvmf_request *req)
{
	struct spdk_nvme_kv_cmd *cmd = (struct spdk_nvme_kv_cmd *)&req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;
	struct spdk_nvme_kv_key_t key;
	int rc;

	spdk_nvme_kv_cmd_get_key(cmd, &key);
	rc = spdk_bdev_kv_exist(desc, ch, key.kl, key.key,
				nvmf_bdev_ctrlr_complete_cmd, req);
	if (spdk_unlikely(rc)) {
		if (rc == -ENOMEM) {
			nvmf_bdev_ctrl_queue_io(req, bdev, ch, nvmf_ctrlr_process_io_cmd_resubmit, req);
			return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
		}
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
}

int
nvmf_bdev_ctrlr_delete_cmd(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
			   struct spdk_io_channel *ch, struct spdk_nvmf_request *req)
{
	struct spdk_nvme_kv_cmd *cmd = (struct spdk_nvme_kv_cmd *)&req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;
	struct spdk_nvme_kv_key_t key;
	int rc;

	spdk_nvme_kv_cmd_get_key(cmd, &key);
	rc = spdk_bdev_kv_delete(desc, ch, key.kl, key.key,
				 nvmf_bdev_ctrlr_complete_cmd, req);
	if (spdk_unlikely(rc)) {
		if (rc == -ENOMEM) {
			nvmf_bdev_ctrl_queue_io(req, bdev, ch, nvmf_ctrlr_process_io_cmd_resubmit, req);
			return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
		}
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
}

struct nvmf_bdev_ctrlr_unmap {
	struct spdk_nvmf_request	*req;
	uint32_t			count;
	struct spdk_bdev_desc		*desc;
	struct spdk_bdev		*bdev;
	struct spdk_io_channel		*ch;
	uint32_t			range_index;
};

static void
nvmf_bdev_ctrlr_unmap_cpl(struct spdk_bdev_io *bdev_io, bool success,
			  void *cb_arg)
{
	struct nvmf_bdev_ctrlr_unmap *unmap_ctx = cb_arg;
	struct spdk_nvmf_request	*req = unmap_ctx->req;
	struct spdk_nvme_cpl		*response = &req->rsp->nvme_cpl;
	int				sc, sct;
	uint32_t			cdw0;

	unmap_ctx->count--;

	if (response->status.sct == SPDK_NVME_SCT_GENERIC &&
	    response->status.sc == SPDK_NVME_SC_SUCCESS) {
		spdk_bdev_io_get_nvme_status(bdev_io, &cdw0, &sct, &sc);
		response->cdw0 = cdw0;
		response->status.sc = sc;
		response->status.sct = sct;
	}

	if (unmap_ctx->count == 0) {
		spdk_nvmf_request_complete(req);
		free(unmap_ctx);
	}
	spdk_bdev_free_io(bdev_io);
}

static int nvmf_bdev_ctrlr_unmap(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
				 struct spdk_io_channel *ch, struct spdk_nvmf_request *req,
				 struct nvmf_bdev_ctrlr_unmap *unmap_ctx);
static void
nvmf_bdev_ctrlr_unmap_resubmit(void *arg)
{
	struct nvmf_bdev_ctrlr_unmap *unmap_ctx = arg;
	struct spdk_nvmf_request *req = unmap_ctx->req;
	struct spdk_bdev_desc *desc = unmap_ctx->desc;
	struct spdk_bdev *bdev = unmap_ctx->bdev;
	struct spdk_io_channel *ch = unmap_ctx->ch;

	nvmf_bdev_ctrlr_unmap(bdev, desc, ch, req, unmap_ctx);
}

static int
nvmf_bdev_ctrlr_unmap(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
		      struct spdk_io_channel *ch, struct spdk_nvmf_request *req,
		      struct nvmf_bdev_ctrlr_unmap *unmap_ctx)
{
	uint16_t nr, i;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;
	struct spdk_iov_xfer ix;
	uint64_t lba;
	uint32_t lba_count;
	int rc;

	nr = cmd->cdw10_bits.dsm.nr + 1;
	if (nr * sizeof(struct spdk_nvme_dsm_range) > req->length) {
		SPDK_ERRLOG("Dataset Management number of ranges > SGL length\n");
		response->status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (unmap_ctx == NULL) {
		unmap_ctx = calloc(1, sizeof(*unmap_ctx));
		if (!unmap_ctx) {
			response->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		}

		unmap_ctx->req = req;
		unmap_ctx->desc = desc;
		unmap_ctx->ch = ch;
		unmap_ctx->bdev = bdev;

		response->status.sct = SPDK_NVME_SCT_GENERIC;
		response->status.sc = SPDK_NVME_SC_SUCCESS;
	} else {
		unmap_ctx->count--;	/* dequeued */
	}

	spdk_iov_xfer_init(&ix, req->iov, req->iovcnt);

	for (i = unmap_ctx->range_index; i < nr; i++) {
		struct spdk_nvme_dsm_range dsm_range = { 0 };

		spdk_iov_xfer_to_buf(&ix, &dsm_range, sizeof(dsm_range));

		lba = dsm_range.starting_lba;
		lba_count = dsm_range.length;

		unmap_ctx->count++;

		rc = spdk_bdev_unmap_blocks(desc, ch, lba, lba_count,
					    nvmf_bdev_ctrlr_unmap_cpl, unmap_ctx);
		if (rc) {
			if (rc == -ENOMEM) {
				nvmf_bdev_ctrl_queue_io(req, bdev, ch, nvmf_bdev_ctrlr_unmap_resubmit, unmap_ctx);
				/* Unmap was not yet submitted to bdev */
				/* unmap_ctx->count will be decremented when the request is dequeued */
				return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
			}
			response->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
			unmap_ctx->count--;
			/* We can't return here - we may have to wait for any other
				* unmaps already sent to complete */
			break;
		}
		unmap_ctx->range_index++;
	}

	if (unmap_ctx->count == 0) {
		free(unmap_ctx);
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
}

int
nvmf_bdev_ctrlr_dsm_cmd(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
			struct spdk_io_channel *ch, struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;

	if (cmd->cdw11_bits.dsm.ad) {
		return nvmf_bdev_ctrlr_unmap(bdev, desc, ch, req, NULL);
	}

	response->status.sct = SPDK_NVME_SCT_GENERIC;
	response->status.sc = SPDK_NVME_SC_SUCCESS;
	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

int
nvmf_bdev_ctrlr_copy_cmd(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
			 struct spdk_io_channel *ch, struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;
	uint64_t sdlba = ((uint64_t)cmd->cdw11 << 32) + cmd->cdw10;
	struct spdk_nvme_scc_source_range range = { 0 };
	struct spdk_iov_xfer ix;
	int rc;

	SPDK_DEBUGLOG(nvmf, "Copy command: SDLBA %lu, NR %u, desc format %u, PRINFOR %u, "
		      "DTYPE %u, STCW %u, PRINFOW %u, FUA %u, LR %u\n",
		      sdlba,
		      cmd->cdw12_bits.copy.nr,
		      cmd->cdw12_bits.copy.df,
		      cmd->cdw12_bits.copy.prinfor,
		      cmd->cdw12_bits.copy.dtype,
		      cmd->cdw12_bits.copy.stcw,
		      cmd->cdw12_bits.copy.prinfow,
		      cmd->cdw12_bits.copy.fua,
		      cmd->cdw12_bits.copy.lr);

	if (spdk_unlikely(req->length != (cmd->cdw12_bits.copy.nr + 1) *
			  sizeof(struct spdk_nvme_scc_source_range))) {
		response->status.sct = SPDK_NVME_SCT_GENERIC;
		response->status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	/*
	 * We support only one source range, and rely on this with the xfer
	 * below.
	 */
	if (cmd->cdw12_bits.copy.nr > 0) {
		response->status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		response->status.sc = SPDK_NVME_SC_CMD_SIZE_LIMIT_SIZE_EXCEEDED;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (cmd->cdw12_bits.copy.df != 0) {
		response->status.sct = SPDK_NVME_SCT_GENERIC;
		response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	spdk_iov_xfer_init(&ix, req->iov, req->iovcnt);
	spdk_iov_xfer_to_buf(&ix, &range, sizeof(range));

	rc = spdk_bdev_copy_blocks(desc, ch, sdlba, range.slba, range.nlb + 1,
				   nvmf_bdev_ctrlr_complete_cmd, req);
	if (spdk_unlikely(rc)) {
		if (rc == -ENOMEM) {
			nvmf_bdev_ctrl_queue_io(req, bdev, ch, nvmf_ctrlr_process_io_cmd_resubmit, req);
			return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
		}

		response->status.sct = SPDK_NVME_SCT_GENERIC;
		response->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
}

int
nvmf_bdev_ctrlr_nvme_passthru_io(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
				 struct spdk_io_channel *ch, struct spdk_nvmf_request *req)
{
	int rc;

	if (spdk_unlikely(req->iovcnt != 1)) {
		req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
		req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		req->rsp->nvme_cpl.status.dnr = 1;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	rc = spdk_bdev_nvme_io_passthru(desc, ch, &req->cmd->nvme_cmd, req->iov[0].iov_base, req->length,
					nvmf_bdev_ctrlr_complete_cmd, req);
	if (spdk_unlikely(rc)) {
		if (rc == -ENOMEM) {
			nvmf_bdev_ctrl_queue_io(req, bdev, ch, nvmf_ctrlr_process_io_cmd_resubmit, req);
			return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
		}
		req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
		req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INVALID_OPCODE;
		req->rsp->nvme_cpl.status.dnr = 1;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
}

int
spdk_nvmf_bdev_ctrlr_nvme_passthru_admin(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
		struct spdk_io_channel *ch, struct spdk_nvmf_request *req,
		spdk_nvmf_nvme_passthru_cmd_cb cb_fn)
{
	int rc;

	if (spdk_unlikely(req->iovcnt != 1)) {
		req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
		req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		req->rsp->nvme_cpl.status.dnr = 1;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	req->cmd_cb_fn = cb_fn;

	rc = spdk_bdev_nvme_admin_passthru(desc, ch, &req->cmd->nvme_cmd, req->iov[0].iov_base, req->length,
					   nvmf_bdev_ctrlr_complete_admin_cmd, req);
	if (spdk_unlikely(rc)) {
		if (rc == -ENOMEM) {
			nvmf_bdev_ctrl_queue_io(req, bdev, ch, nvmf_ctrlr_process_admin_cmd_resubmit, req);
			return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
		}
		req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
		if (rc == -ENOTSUP) {
			req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INVALID_OPCODE;
		} else {
			req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		}

		req->rsp->nvme_cpl.status.dnr = 1;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
}

static void
nvmf_bdev_ctrlr_complete_abort_cmd(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_nvmf_request *req = cb_arg;

	if (success) {
		req->rsp->nvme_cpl.cdw0 &= ~1U;
	}

	spdk_nvmf_request_complete(req);
	spdk_bdev_free_io(bdev_io);
}

int
spdk_nvmf_bdev_ctrlr_abort_cmd(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
			       struct spdk_io_channel *ch, struct spdk_nvmf_request *req,
			       struct spdk_nvmf_request *req_to_abort)
{
	int rc;

	assert((req->rsp->nvme_cpl.cdw0 & 1U) != 0);

	rc = spdk_bdev_abort(desc, ch, req_to_abort, nvmf_bdev_ctrlr_complete_abort_cmd, req);
	if (spdk_likely(rc == 0)) {
		return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
	} else if (rc == -ENOMEM) {
		nvmf_bdev_ctrl_queue_io(req, bdev, ch, nvmf_ctrlr_process_admin_cmd_resubmit, req);
		return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
	} else {
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}
}

bool
nvmf_bdev_ctrlr_get_dif_ctx(struct spdk_bdev *bdev, struct spdk_nvme_cmd *cmd,
			    struct spdk_dif_ctx *dif_ctx)
{
	uint32_t init_ref_tag, dif_check_flags = 0;
	int rc;

	if (spdk_bdev_get_md_size(bdev) == 0) {
		return false;
	}

	/* Initial Reference Tag is the lower 32 bits of the start LBA. */
	init_ref_tag = (uint32_t)from_le64(&cmd->cdw10);

	if (spdk_bdev_is_dif_check_enabled(bdev, SPDK_DIF_CHECK_TYPE_REFTAG)) {
		dif_check_flags |= SPDK_DIF_FLAGS_REFTAG_CHECK;
	}

	if (spdk_bdev_is_dif_check_enabled(bdev, SPDK_DIF_CHECK_TYPE_GUARD)) {
		dif_check_flags |= SPDK_DIF_FLAGS_GUARD_CHECK;
	}

	rc = spdk_dif_ctx_init(dif_ctx,
			       spdk_bdev_get_block_size(bdev),
			       spdk_bdev_get_md_size(bdev),
			       spdk_bdev_is_md_interleaved(bdev),
			       spdk_bdev_is_dif_head_of_md(bdev),
			       spdk_bdev_get_dif_type(bdev),
			       dif_check_flags,
			       init_ref_tag, 0, 0, 0, 0);

	return (rc == 0) ? true : false;
}

static void
nvmf_bdev_ctrlr_zcopy_start_complete(struct spdk_bdev_io *bdev_io, bool success,
				     void *cb_arg)
{
	struct spdk_nvmf_request	*req = cb_arg;
	struct iovec *iov;
	int iovcnt = 0;

	if (spdk_unlikely(!success)) {
		int                     sc = 0, sct = 0;
		uint32_t                cdw0 = 0;
		struct spdk_nvme_cpl    *response = &req->rsp->nvme_cpl;
		spdk_bdev_io_get_nvme_status(bdev_io, &cdw0, &sct, &sc);

		response->cdw0 = cdw0;
		response->status.sc = sc;
		response->status.sct = sct;

		spdk_bdev_free_io(bdev_io);
		spdk_nvmf_request_complete(req);
		return;
	}

	spdk_bdev_io_get_iovec(bdev_io, &iov, &iovcnt);

	assert(iovcnt <= NVMF_REQ_MAX_BUFFERS);
	assert(iovcnt > 0);

	req->iovcnt = iovcnt;

	assert(req->iov == iov);

	/* backward compatible */
	req->data = req->iov[0].iov_base;

	req->zcopy_bdev_io = bdev_io; /* Preserve the bdev_io for the end zcopy */

	spdk_nvmf_request_complete(req);
	/* Don't free the bdev_io here as it is needed for the END ZCOPY */
}

int
nvmf_bdev_ctrlr_zcopy_start(struct spdk_bdev *bdev,
			    struct spdk_bdev_desc *desc,
			    struct spdk_io_channel *ch,
			    struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;
	uint64_t bdev_num_blocks = spdk_bdev_get_num_blocks(bdev);
	uint32_t block_size = spdk_bdev_get_block_size(bdev);
	uint64_t start_lba;
	uint64_t num_blocks;
	int rc;

	nvmf_bdev_ctrlr_get_rw_params(&req->cmd->nvme_cmd, &start_lba, &num_blocks);

	if (spdk_unlikely(!nvmf_bdev_ctrlr_lba_in_range(bdev_num_blocks, start_lba, num_blocks))) {
		SPDK_ERRLOG("end of media\n");
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_LBA_OUT_OF_RANGE;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (spdk_unlikely(num_blocks * block_size > req->length)) {
		SPDK_ERRLOG("Read NLB %" PRIu64 " * block size %" PRIu32 " > SGL length %" PRIu32 "\n",
			    num_blocks, block_size, req->length);
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	bool populate = (req->cmd->nvme_cmd.opc == SPDK_NVME_OPC_READ) ? true : false;

	rc = spdk_bdev_zcopy_start(desc, ch, req->iov, req->iovcnt, start_lba,
				   num_blocks, populate, nvmf_bdev_ctrlr_zcopy_start_complete, req);
	if (spdk_unlikely(rc != 0)) {
		if (rc == -ENOMEM) {
			nvmf_bdev_ctrl_queue_io(req, bdev, ch, nvmf_ctrlr_process_io_cmd_resubmit, req);
			return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
		}
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
}

static void
nvmf_bdev_ctrlr_zcopy_end_complete(struct spdk_bdev_io *bdev_io, bool success,
				   void *cb_arg)
{
	struct spdk_nvmf_request	*req = cb_arg;

	if (spdk_unlikely(!success)) {
		int                     sc = 0, sct = 0;
		uint32_t                cdw0 = 0;
		struct spdk_nvme_cpl    *response = &req->rsp->nvme_cpl;
		spdk_bdev_io_get_nvme_status(bdev_io, &cdw0, &sct, &sc);

		response->cdw0 = cdw0;
		response->status.sc = sc;
		response->status.sct = sct;
	}

	spdk_bdev_free_io(bdev_io);
	req->zcopy_bdev_io = NULL;
	spdk_nvmf_request_complete(req);
}

void
nvmf_bdev_ctrlr_zcopy_end(struct spdk_nvmf_request *req, bool commit)
{
	int rc __attribute__((unused));

	rc = spdk_bdev_zcopy_end(req->zcopy_bdev_io, commit, nvmf_bdev_ctrlr_zcopy_end_complete, req);

	/* The only way spdk_bdev_zcopy_end() can fail is if we pass a bdev_io type that isn't ZCOPY */
	assert(rc == 0);
}
