
#include <format>
#include <infiniband/verbs.h>
#include <string>
#include "limen/format.hpp"

namespace limen 
{
    std::string gid_to_str(const ibv_gid* gid)
    {
        std::string res = "";

        //  ibv_gid has uint8_t raw[16], the pretty-print has 
        //  8 sections to it so chunk & print as hex
        for (int i = 0; i < 16; i+=2)
        {
            res+= std::format("{:02x}",gid->raw[i]);
            res+= std::format("{:02x}",gid->raw[i+1]);

            if (i%2==0 && i!=14)
            {
                res+= ":";
            }
        }

        return res;
    }

    int str_to_gid(const std::string& str, ibv_gid* gid)
    {
        // "0000:0000:0000:0000:0000:ffff:c0a8:6401"
        unsigned int g[16];

        if (sscanf(str.c_str(),
            "%2x%2x:%2x%2x:%2x%2x:%2x%2x:%2x%2x:%2x%2x:%2x%2x:%2x%2x",
            &g[0],&g[1],&g[2],&g[3],&g[4],&g[5],&g[6],&g[7],
            &g[8],&g[9],&g[10],&g[11],&g[12],&g[13],&g[14],&g[15]) != 16)
        {
            //  did not parse enough values
            return 1;
        }

        //  pack into gid
        for (int i = 0; i < 16; i++)
        {
            gid->raw[i] = g[i];
        }
        return 0;
    }


    std::string qp_state_to_str(ibv_qp_state state)
    {

        switch (state) {
            case IBV_QPS_RESET: return "IBV_QPS_RESET";
            case IBV_QPS_INIT:  return "IBV_QPS_INIT";
            case IBV_QPS_RTR:   return "IBV_QPS_RTR";
            case IBV_QPS_RTS:   return "IBV_QPS_RTS";
            case IBV_QPS_SQD:   return "IBV_QPS_SQD";
            case IBV_QPS_SQE:   return "IBV_QPS_SQE";
            case IBV_QPS_ERR:   return "IBV_QPS_ERR";
            default:            return "UNKNOWN";
        }
    }

    const char* wc_status_name(enum ibv_wc_status s)
    {
        switch (s) {
        case IBV_WC_SUCCESS:            return "SUCCESS";
        case IBV_WC_LOC_LEN_ERR:        return "LOC_LEN_ERR";
        case IBV_WC_LOC_QP_OP_ERR:      return "LOC_QP_OP_ERR";
        case IBV_WC_LOC_EEC_OP_ERR:     return "LOC_EEC_OP_ERR";
        case IBV_WC_LOC_PROT_ERR:       return "LOC_PROT_ERR";
        case IBV_WC_WR_FLUSH_ERR:       return "WR_FLUSH_ERR";
        case IBV_WC_MW_BIND_ERR:        return "MW_BIND_ERR";
        case IBV_WC_BAD_RESP_ERR:       return "BAD_RESP_ERR";
        case IBV_WC_LOC_ACCESS_ERR:     return "LOC_ACCESS_ERR";
        case IBV_WC_REM_INV_REQ_ERR:    return "REM_INV_REQ_ERR";
        case IBV_WC_REM_ACCESS_ERR:     return "REM_ACCESS_ERR";
        case IBV_WC_REM_OP_ERR:         return "REM_OP_ERR";
        case IBV_WC_RETRY_EXC_ERR:      return "RETRY_EXC_ERR";
        case IBV_WC_RNR_RETRY_EXC_ERR:  return "RNR_RETRY_EXC_ERR";
        case IBV_WC_LOC_RDD_VIOL_ERR:   return "LOC_RDD_VIOL_ERR";
        case IBV_WC_REM_INV_RD_REQ_ERR: return "REM_INV_RD_REQ_ERR";
        case IBV_WC_REM_ABORT_ERR:      return "REM_ABORT_ERR";
        case IBV_WC_INV_EECN_ERR:       return "INV_EECN_ERR";
        case IBV_WC_INV_EEC_STATE_ERR:  return "INV_EEC_STATE_ERR";
        case IBV_WC_FATAL_ERR:          return "FATAL_ERR";
        case IBV_WC_RESP_TIMEOUT_ERR:   return "RESP_TIMEOUT_ERR";
        case IBV_WC_GENERAL_ERR:        return "GENERAL_ERR";
        case IBV_WC_TM_ERR:             return "TM_ERR";
        case IBV_WC_TM_RNDV_INCOMPLETE: return "TM_RNDV_INCOMPLETE";
        }
        return "UNKNOWN";
    }

    const char* wc_opcode_str(enum ibv_wc_opcode opcode)
    {
        switch (opcode) {
            case IBV_WC_SEND:                     return "SEND";
            case IBV_WC_RDMA_WRITE:               return "RDMA_WRITE";
            case IBV_WC_RDMA_READ:                return "RDMA_READ";
            case IBV_WC_COMP_SWAP:                return "COMP_SWAP";
            case IBV_WC_FETCH_ADD:                return "FETCH_ADD";
            case IBV_WC_BIND_MW:                  return "BIND_MW";
            case IBV_WC_LOCAL_INV:                return "LOCAL_INV";
            case IBV_WC_TSO:                      return "TSO";
            case IBV_WC_RECV:                     return "RECV";
            case IBV_WC_RECV_RDMA_WITH_IMM:       return "RECV_RDMA_WITH_IMM";
            default:                              return "UNKNOWN_OPCODE";
        }
    }

    std::string wc_to_str(ibv_wc *wc)
    {
        if (wc->status == IBV_WC_SUCCESS)
        {
            //  successful
            if (wc->opcode == IBV_WC_RECV)
            {
                return std::format(
                    "completion: wr_id={:#016x} opcode={} status={} byte_len={}",
                    wc->wr_id,
                    wc_opcode_str(wc->opcode),
                    wc_status_name(wc->status),
                    wc->byte_len
                );
            }
            else {
                return std::format(
                    "completion: wr_id={:#016x} opcode={} status={}",
                    wc->wr_id,
                    wc_opcode_str(wc->opcode),
                    wc_status_name(wc->status)
                );
            }
        }
        else 
        {
            //  unsuccessful
            return std::format(
                "completion: wr_id={:#016x} status={} vendor_err={:#08x}", 
                wc->wr_id,
                wc_status_name(wc->status),
                wc->vendor_err
            );
        }

    }

}