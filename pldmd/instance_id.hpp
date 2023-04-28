#pragma once

#include "libpldm/instance-id.h"

#include <cerrno>
#include <cstdint>
#include <exception>
#include <iostream>
#include <string>
#include <system_error>

namespace pldm
{

/** @class InstanceId
 *  @brief Implementation of PLDM instance id as per DSP0240 v1.0.0
 */
class InstanceIdDb
{
  public:
    InstanceIdDb()
    {
        int rc = pldm_instance_db_init_default(&pldmInstanceIdDb);
        if (rc)
        {
            throw std::system_category().default_error_condition(rc);
        }
    }

    InstanceIdDb(const std::string& path)
    {
        int rc = pldm_instance_db_init(&pldmInstanceIdDb, path.c_str());
        if (rc)
        {
            throw std::system_category().default_error_condition(rc);
        }
    }

    ~InstanceIdDb()
    {
        int rc = pldm_instance_db_destroy(pldmInstanceIdDb);
        if (rc)
        {
            std::cout << "pldm_instance_db_destroy failed, rc =" << rc << "\n";
        }
    }

    /** @brief Allocate an instance ID for the given terminus
     *  @return - PLDM instance id or -EAGAIN if there are no available instance
     * IDs
     */
    uint8_t next(uint8_t tid)
    {
        uint8_t id;
        int rc = pldm_instance_id_alloc(pldmInstanceIdDb, tid, &id);

        if (rc == -EAGAIN)
        {
            throw std::runtime_error("No free instance ids");
        }

        if (rc)
        {
            throw std::system_category().default_error_condition(rc);
        }

        return id;
    }

    /** @brief Mark an instance id as unused
     *  @param[in] tid - the terminus ID the instance ID is associated with
     *  @param[in] instanceId - PLDM instance id to be freed
     */
    void free(uint8_t tid, uint8_t instanceId)
    {
        int rc = pldm_instance_id_free(pldmInstanceIdDb, tid, instanceId);
        if (rc == -EINVAL)
        {
            throw std::runtime_error("Invalid instance ID");
        }
        if (rc)
        {
            throw std::system_category().default_error_condition(rc);
        }
    }

  private:
    struct pldm_instance_db* pldmInstanceIdDb = nullptr;
};

} // namespace pldm
