#include "oem_ibm_handler.hpp"

#include "collect_slot_vpd.hpp"
#include "file_io_type_lid.hpp"
#include "libpldmresponder/file_io.hpp"
#include "libpldmresponder/pdr_utils.hpp"

#include <libpldm/entity.h>
#include <libpldm/oem/ibm/entity.h>
#include <libpldm/state_set.h>

#include <phosphor-logging/lg2.hpp>
#include <xyz/openbmc_project/State/BMC/client.hpp>

#include <regex>

PHOSPHOR_LOG2_USING;

using namespace pldm::pdr;
using namespace pldm::utils;

namespace pldm
{
namespace responder
{
namespace oem_ibm_platform
{
int pldm::responder::oem_ibm_platform::Handler::
    getOemStateSensorReadingsHandler(
        pldm::pdr::EntityType entityType, EntityInstance entityInstance,
        ContainerID containerId, StateSetId stateSetId,
        CompositeCount compSensorCnt, uint16_t /*sensorId*/,
        std::vector<get_sensor_state_field>& stateField)
{
    auto& entityAssociationMap = getAssociateEntityMap();
    int rc = PLDM_SUCCESS;
    stateField.clear();

    for (size_t i = 0; i < compSensorCnt; i++)
    {
        uint8_t sensorOpState{};
        uint8_t presentState = PLDM_SENSOR_UNKNOWN;
        if (entityType == PLDM_OEM_IBM_ENTITY_FIRMWARE_UPDATE &&
            stateSetId == PLDM_OEM_IBM_BOOT_STATE)
        {
            sensorOpState = fetchBootSide(entityInstance, codeUpdate);
        }
        else if (entityType == PLDM_ENTITY_SLOT &&
                 stateSetId == PLDM_OEM_IBM_PCIE_SLOT_SENSOR_STATE)
        {
            for (const auto& [key, value] : entityAssociationMap)
            {
                if (value.entity_type == entityType &&
                    value.entity_instance_num == entityInstance &&
                    value.entity_container_id == containerId)
                {
                    sensorOpState = slotHandler->fetchSlotSensorState(key);
                    break;
                }
            }
        }
        else if (entityType == PLDM_OEM_IBM_ENTITY_REAL_SAI &&
                 stateSetId == PLDM_STATE_SET_OPERATIONAL_FAULT_STATUS)
        {
            sensorOpState = fetchRealSAIStatus();
            presentState = PLDM_SENSOR_NORMAL;
        }
        else if ((entityType == PLDM_ENTITY_MEMORY_MODULE) &&
                 (stateSetId == PLDM_OEM_IBM_SBE_DUMP_UPDATE_STATE))
        {
            sensorOpState = fetchDimmStateSensor(entityInstance);
            stateField.push_back({PLDM_SENSOR_ENABLED, PLDM_SENSOR_UNKNOWN,
                                  PLDM_SENSOR_UNKNOWN, sensorOpState});
            break;
        }
        else
        {
            rc = PLDM_PLATFORM_INVALID_STATE_VALUE;
            break;
        }
        stateField.push_back({PLDM_SENSOR_ENABLED, presentState,
                              PLDM_SENSOR_UNKNOWN, sensorOpState});
    }
    return rc;
}

std::vector<InstanceInfo>
    pldm::responder::oem_ibm_platform::Handler::generateProcAndDcmIDs()
{
    std::vector<InstanceInfo> dcmProcInfo;
    std::vector<std::string> procObjectPaths;
    procObjectPaths = getProcObjectPaths();

    for (const auto& entity_path : procObjectPaths)
    {
        if (entity_path.rfind('/') != std::string::npos)
        {
            char pId = entity_path.back();
            auto procId = pId - 48;
            char id = entity_path.at(61);
            auto dcmId = id - 48;

            dcmProcInfo.emplace_back(InstanceInfo{static_cast<uint8_t>(procId),
                                                  static_cast<uint8_t>(dcmId)});
        }
    }
    return dcmProcInfo;
}

std::vector<uint16_t>
    pldm::responder::oem_ibm_platform::Handler::generateDimmIds()
{
    std::vector<uint16_t> dimmInfo;
    std::vector<std::string> dimmObjPaths;
    dimmObjPaths = getDimmObjectPaths();

    for (const auto& entity_path : dimmObjPaths)
    {
        if (entity_path.rfind('/') != std::string::npos)
        {
            auto dimmId = atoi(entity_path.substr(62).c_str());
            dimmInfo.emplace_back(dimmId);
        }
    }
    return dimmInfo;
}

int pldm::responder::oem_ibm_platform::Handler::
    oemSetNumericEffecterValueHandler(
        uint16_t entityType, uint16_t entityInstance,
        uint16_t effecterSemanticId, uint8_t effecterDataSize,
        uint8_t* effecterValue, real32_t effecterOffset,
        real32_t effecterResolution, uint16_t effecterId)
{
    int rc = PLDM_SUCCESS;

    if (effecterSemanticId == PLDM_OEM_IBM_SBE_SEMANTIC_ID &&
        effecterDataSize == PLDM_EFFECTER_DATA_SIZE_UINT32)
    {
        uint32_t currentValue =
            *(reinterpret_cast<uint32_t*>(&effecterValue[0]));
        auto rawValue = static_cast<uint32_t>(
            round(currentValue - effecterOffset) / effecterResolution);
        pldm::utils::PropertyValue value;
        value = rawValue;

        if (entityType == PLDM_ENTITY_PROC)
        {
            for (auto& [key, value] : instanceMap)
            {
                if (key == effecterId)
                {
                    entityInstance = (value.dcmId * 2) +
                                     value.procId; // failingUintId
                }
            }
        }

        else if (entityType == PLDM_ENTITY_MEMORY_MODULE)
        {
            for (auto& [key, value] : instanceDimmMap)
            {
                if (key == effecterId)
                {
                    entityInstance = value; // failingUintId
                }
            }
        }

        else
        {
            error(
                "Invalid entity type received: {ENTITY_TYPE} for entityInstance '{INSTANCE}'",
                "ENTITY_TYPE", entityType, "INSTANCE", entityInstance);
            return PLDM_ERROR_INVALID_DATA;
        }
        info(
            "Processing setNumericEffecter on ID {ID} for effecter type {TYPE} and entity instance {INST}",
            "ID", effecterId, "TYPE", entityType, "INST", entityInstance);
        rc = setNumericEffecter(entityInstance, value, entityType);
    }
    return rc;
}

int pldm::responder::oem_ibm_platform::Handler::
    oemSetStateEffecterStatesHandler(
        uint16_t entityType, uint16_t stateSetId, uint8_t compEffecterCnt,
        std::vector<set_effecter_state_field>& stateField, uint16_t effecterId)
{
    int rc = PLDM_SUCCESS;
    auto& entityAssociationMap = getAssociateEntityMap();

    for (uint8_t currState = 0; currState < compEffecterCnt; ++currState)
    {
        if (stateField[currState].set_request == PLDM_REQUEST_SET)
        {
            if (entityType == PLDM_OEM_IBM_ENTITY_FIRMWARE_UPDATE &&
                stateSetId == PLDM_OEM_IBM_FIRMWARE_UPDATE_STATE)
            {
                if (stateField[currState].effecter_state ==
                    uint8_t(CodeUpdateState::START))
                {
                    info("Received Start Update Request From PHYP");
                    codeUpdate->setCodeUpdateProgress(true);
                    startUpdateEvent =
                        std::make_unique<sdeventplus::source::Defer>(
                            event,
                            std::bind(std::mem_fn(&oem_ibm_platform::Handler::
                                                      _processStartUpdate),
                                      this, std::placeholders::_1));
                }
                else if (stateField[currState].effecter_state ==
                         uint8_t(CodeUpdateState::END))
                {
                    info("Received End Update Request From PHYP");
                    rc = PLDM_SUCCESS;
                    assembleImageEvent = std::make_unique<
                        sdeventplus::source::Defer>(
                        event,
                        std::bind(
                            std::mem_fn(
                                &oem_ibm_platform::Handler::_processEndUpdate),
                            this, std::placeholders::_1));

                    // sendCodeUpdateEvent(effecterId, END, START);
                }
                else if (stateField[currState].effecter_state ==
                         uint8_t(CodeUpdateState::ABORT))
                {
                    info("Received Abort Update Request From PHYP");
                    codeUpdate->setCodeUpdateProgress(false);
                    codeUpdate->clearDirPath(LID_STAGING_DIR);
                    auto sensorId = codeUpdate->getFirmwareUpdateSensor();
                    sendStateSensorEvent(sensorId, PLDM_STATE_SENSOR_STATE, 0,
                                         uint8_t(CodeUpdateState::ABORT),
                                         uint8_t(CodeUpdateState::START));
                    // sendCodeUpdateEvent(effecterId, ABORT, END);
                }
                else if (stateField[currState].effecter_state ==
                         uint8_t(CodeUpdateState::ACCEPT))
                {
                    auto sensorId = codeUpdate->getFirmwareUpdateSensor();
                    sendStateSensorEvent(sensorId, PLDM_STATE_SENSOR_STATE, 0,
                                         uint8_t(CodeUpdateState::ACCEPT),
                                         uint8_t(CodeUpdateState::END));
                    // TODO Set new Dbus property provided by code update app
                    // sendCodeUpdateEvent(effecterId, ACCEPT, END);
                }
                else if (stateField[currState].effecter_state ==
                         uint8_t(CodeUpdateState::REJECT))
                {
                    auto sensorId = codeUpdate->getFirmwareUpdateSensor();
                    sendStateSensorEvent(sensorId, PLDM_STATE_SENSOR_STATE, 0,
                                         uint8_t(CodeUpdateState::REJECT),
                                         uint8_t(CodeUpdateState::END));
                    // TODO Set new Dbus property provided by code update app
                    // sendCodeUpdateEvent(effecterId, REJECT, END);
                }
            }
            else if (entityType == PLDM_ENTITY_SYSTEM_CHASSIS &&
                     stateSetId == PLDM_OEM_IBM_SYSTEM_POWER_STATE)
            {
                if (stateField[currState].effecter_state == POWER_CYCLE_HARD)
                {
                    systemRebootEvent =
                        std::make_unique<sdeventplus::source::Defer>(
                            event,
                            std::bind(std::mem_fn(&oem_ibm_platform::Handler::
                                                      _processSystemReboot),
                                      this, std::placeholders::_1));
                }
            }
            else if (stateSetId == PLDM_OEM_IBM_PCIE_SLOT_EFFECTER_STATE)
            {
                slotHandler->enableSlot(effecterId, entityAssociationMap,
                                        stateField[currState].effecter_state);
            }
            else if (entityType == PLDM_OEM_IBM_CHASSIS_POWER_CONTROLLER &&
                     stateSetId == PLDM_STATE_SET_SYSTEM_POWER_STATE)
            {
                if (stateField[currState].effecter_state ==
                    PLDM_STATE_SET_SYS_POWER_CYCLE_OFF_SOFT_GRACEFUL)
                {
                    processPowerCycleOffSoftGraceful();
                }
                else if (stateField[currState].effecter_state ==
                         PLDM_STATE_SET_SYS_POWER_STATE_OFF_SOFT_GRACEFUL)
                {
                    processPowerOffSoftGraceful();
                }
                else if (stateField[currState].effecter_state ==
                         PLDM_STATE_SET_SYS_POWER_STATE_OFF_HARD_GRACEFUL)
                {
                    processPowerOffHardGraceful();
                }
            }
            else if (entityType == PLDM_OEM_IBM_ENTITY_REAL_SAI &&
                     stateSetId == PLDM_STATE_SET_OPERATIONAL_FAULT_STATUS)
            {
                turnOffRealSAIEffecter();
            }
            else if (entityType == PLDM_OEM_IBM_ENTITY_FIRMWARE_UPDATE &&
                     stateSetId == PLDM_OEM_IBM_BOOT_SIDE_RENAME)
            {
                if (stateField[currState].effecter_state ==
                    PLDM_BOOT_SIDE_HAS_BEEN_RENAMED)
                {
                    codeUpdate->processRenameEvent();
                }
            }
            else
            {
                rc = PLDM_PLATFORM_SET_EFFECTER_UNSUPPORTED_SENSORSTATE;
            }
        }
        if (rc != PLDM_SUCCESS)
        {
            break;
        }
    }
    return rc;
}

void buildAllSystemPowerStateEffecterPDR(
    oem_ibm_platform::Handler* platformHandler, uint16_t entityType,
    uint16_t entityInstance, uint16_t stateSetID, pdr_utils::Repo& repo)
{
    size_t pdrSize = 0;
    pdrSize = sizeof(pldm_state_effecter_pdr) +
              sizeof(state_effecter_possible_states);
    std::vector<uint8_t> entry{};
    entry.resize(pdrSize);
    pldm_state_effecter_pdr* pdr =
        reinterpret_cast<pldm_state_effecter_pdr*>(entry.data());
    if (!pdr)
    {
        error("Failed to get record by PDR type, ERROR:{ERR}", "ERR", lg2::hex,
              static_cast<unsigned>(PLDM_PLATFORM_INVALID_EFFECTER_ID));
        return;
    }
    pdr->hdr.record_handle = 0;
    pdr->hdr.version = 1;
    pdr->hdr.type = PLDM_STATE_EFFECTER_PDR;
    pdr->hdr.record_change_num = 0;
    pdr->hdr.length = sizeof(pldm_state_effecter_pdr) - sizeof(pldm_pdr_hdr);
    pdr->terminus_handle = TERMINUS_HANDLE;
    pdr->effecter_id = platformHandler->getNextEffecterId();
    pdr->entity_type = entityType;
    pdr->entity_instance = entityInstance;
    pdr->container_id = 1;
    pdr->effecter_semantic_id = 0;
    pdr->effecter_init = PLDM_NO_INIT;
    pdr->has_description_pdr = false;
    pdr->composite_effecter_count = 1;

    auto* possibleStatesPtr = pdr->possible_states;
    auto possibleStates =
        reinterpret_cast<state_effecter_possible_states*>(possibleStatesPtr);
    possibleStates->state_set_id = stateSetID;
    possibleStates->possible_states_size = 2;
    auto state =
        reinterpret_cast<state_effecter_possible_states*>(possibleStates);
    state->states[0].byte = 128;
    state->states[1].byte = 6;
    pldm::responder::pdr_utils::PdrEntry pdrEntry{};
    pdrEntry.data = entry.data();
    pdrEntry.size = pdrSize;
    repo.addRecord(pdrEntry);
}

void attachOemEntityToEntityAssociationPDR(
    oem_ibm_platform::Handler* platformHandler,
    pldm_entity_association_tree* bmcEntityTree,
    const std::string& parentEntityPath, pdr_utils::Repo& repo,
    pldm_entity childEntity)
{
    auto& associatedEntityMap = platformHandler->getAssociateEntityMap();
    if (associatedEntityMap.contains(parentEntityPath))
    {
        // Parent is present in the entity association PDR
        pldm_entity parent_entity = associatedEntityMap.at(parentEntityPath);
        auto parent_node = pldm_entity_association_tree_find_with_locality(
            bmcEntityTree, &parent_entity, false);
        if (!parent_node)
        {
            // parent node not found in the entity association tree,
            // this should not be possible
            error(
                "Parent Entity of type {ENTITY_TYP} not found in the BMC Entity Association tree ",
                "ENTITY_TYP", static_cast<unsigned>(parent_entity.entity_type));
            return;
        }

        uint32_t bmc_record_handle = 0;

        auto lastLocalRecord = pldm_pdr_find_last_in_range(
            repo.getPdr(), BMC_PDR_START_RANGE, BMC_PDR_END_RANGE);
        bmc_record_handle = pldm_pdr_get_record_handle(repo.getPdr(),
                                                       lastLocalRecord);
        uint32_t updatedRecordHdlBmc = 0;
        bool found = false;
        pldm_entity_association_find_parent_entity(
            repo.getPdr(), &parent_entity, false, &updatedRecordHdlBmc, &found);
        if (found)
        {
            pldm_entity_association_pdr_add_contained_entity_to_remote_pdr(
                repo.getPdr(), &childEntity, updatedRecordHdlBmc);
        }
        else
        {
            pldm_entity_association_pdr_create_new(
                repo.getPdr(), bmc_record_handle, &parent_entity, &childEntity,
                &updatedRecordHdlBmc);
        }
    }
}

void buildAllCodeUpdateEffecterPDR(oem_ibm_platform::Handler* platformHandler,
                                   uint16_t entityType, uint16_t entityInstance,
                                   uint16_t stateSetID, pdr_utils::Repo& repo)
{
    size_t pdrSize = 0;
    pdrSize = sizeof(pldm_state_effecter_pdr) +
              sizeof(state_effecter_possible_states);
    std::vector<uint8_t> entry{};
    entry.resize(pdrSize);
    pldm_state_effecter_pdr* pdr =
        reinterpret_cast<pldm_state_effecter_pdr*>(entry.data());
    if (!pdr)
    {
        error("Failed to get record by PDR type, error - {ERROR}", "ERROR",
              lg2::hex,
              static_cast<unsigned>(PLDM_PLATFORM_INVALID_EFFECTER_ID));
        return;
    }
    pdr->hdr.record_handle = 0;
    pdr->hdr.version = 1;
    pdr->hdr.type = PLDM_STATE_EFFECTER_PDR;
    pdr->hdr.record_change_num = 0;
    pdr->hdr.length = sizeof(pldm_state_effecter_pdr) - sizeof(pldm_pdr_hdr);
    pdr->terminus_handle = TERMINUS_HANDLE;
    pdr->effecter_id = platformHandler->getNextEffecterId();
    pdr->entity_type = entityType;
    pdr->entity_instance = entityInstance;
    pdr->container_id = 1;
    pdr->effecter_semantic_id = 0;
    pdr->effecter_init = PLDM_NO_INIT;
    pdr->has_description_pdr = false;
    pdr->composite_effecter_count = 1;

    auto* possibleStatesPtr = pdr->possible_states;
    auto possibleStates =
        reinterpret_cast<state_effecter_possible_states*>(possibleStatesPtr);
    possibleStates->state_set_id = stateSetID;
    possibleStates->possible_states_size = 2;
    auto state =
        reinterpret_cast<state_effecter_possible_states*>(possibleStates);
    if (stateSetID == PLDM_OEM_IBM_BOOT_SIDE_RENAME)
        state->states[0].byte = 6;
    else if (stateSetID == PLDM_OEM_IBM_FIRMWARE_UPDATE_STATE)
        state->states[0].byte = 126;
    else if (stateSetID == PLDM_OEM_IBM_SYSTEM_POWER_STATE)
        state->states[0].byte = 2;
    pldm::responder::pdr_utils::PdrEntry pdrEntry{};
    pdrEntry.data = entry.data();
    pdrEntry.size = pdrSize;
    repo.addRecord(pdrEntry);
}

void buildAllSlotEnableEffecterPDR(oem_ibm_platform::Handler* platformHandler,
                                   pdr_utils::Repo& repo,
                                   const std::vector<std::string>& slotobjpaths)
{
    size_t pdrSize = 0;
    pdrSize = sizeof(pldm_state_effecter_pdr) +
              sizeof(state_effecter_possible_states);
    std::vector<uint8_t> entry{};
    entry.resize(pdrSize);
    pldm_state_effecter_pdr* pdr =
        reinterpret_cast<pldm_state_effecter_pdr*>(entry.data());
    if (!pdr)
    {
        error("Failed to get record by PDR type, ERROR:{ERR}", "ERR", lg2::hex,
              static_cast<unsigned>(PLDM_PLATFORM_INVALID_EFFECTER_ID));
        return;
    }

    auto& associatedEntityMap = platformHandler->getAssociateEntityMap();
    for (const auto& entity_path : slotobjpaths)
    {
        pdr->hdr.record_handle = 0;
        pdr->hdr.version = 1;
        pdr->hdr.type = PLDM_STATE_EFFECTER_PDR;
        pdr->hdr.record_change_num = 0;
        pdr->hdr.length = sizeof(pldm_state_effecter_pdr) -
                          sizeof(pldm_pdr_hdr);
        pdr->terminus_handle = TERMINUS_HANDLE;
        pdr->effecter_id = platformHandler->getNextEffecterId();

        if (entity_path != "" &&
            associatedEntityMap.find(entity_path) != associatedEntityMap.end())
        {
            pdr->entity_type = associatedEntityMap.at(entity_path).entity_type;
            pdr->entity_instance =
                associatedEntityMap.at(entity_path).entity_instance_num;
            pdr->container_id =
                associatedEntityMap.at(entity_path).entity_container_id;
            platformHandler->effecterIdToDbusMap[pdr->effecter_id] =
                entity_path;
        }
        else
        {
            // the slots are not present, dont create the PDR
            continue;
        }
        pdr->effecter_semantic_id = 0;
        pdr->effecter_init = PLDM_NO_INIT;
        pdr->has_description_pdr = false;
        pdr->composite_effecter_count = 1;

        auto* possibleStatesPtr = pdr->possible_states;
        auto possibleStates = reinterpret_cast<state_effecter_possible_states*>(
            possibleStatesPtr);
        possibleStates->state_set_id = PLDM_OEM_IBM_PCIE_SLOT_EFFECTER_STATE;
        possibleStates->possible_states_size = 2;
        auto state =
            reinterpret_cast<state_effecter_possible_states*>(possibleStates);
        state->states[0].byte = 14;
        pldm::responder::pdr_utils::PdrEntry pdrEntry{};
        pdrEntry.data = entry.data();
        pdrEntry.size = pdrSize;
        repo.addRecord(pdrEntry);
    }
}

void buildAllCodeUpdateSensorPDR(oem_ibm_platform::Handler* platformHandler,
                                 uint16_t entityType, uint16_t entityInstance,
                                 uint16_t stateSetID, pdr_utils::Repo& repo)
{
    size_t pdrSize = 0;
    pdrSize = sizeof(pldm_state_sensor_pdr) +
              sizeof(state_sensor_possible_states);
    std::vector<uint8_t> entry{};
    entry.resize(pdrSize);
    pldm_state_sensor_pdr* pdr =
        reinterpret_cast<pldm_state_sensor_pdr*>(entry.data());
    if (!pdr)
    {
        error("Failed to get record by PDR type, error - {ERROR}", "ERROR",
              lg2::hex, static_cast<unsigned>(PLDM_PLATFORM_INVALID_SENSOR_ID));
        return;
    }
    pdr->hdr.record_handle = 0;
    pdr->hdr.version = 1;
    pdr->hdr.type = PLDM_STATE_SENSOR_PDR;
    pdr->hdr.record_change_num = 0;
    pdr->hdr.length = sizeof(pldm_state_sensor_pdr) - sizeof(pldm_pdr_hdr);
    pdr->terminus_handle = TERMINUS_HANDLE;
    pdr->sensor_id = platformHandler->getNextSensorId();
    pdr->entity_type = entityType;
    pdr->entity_instance = entityInstance;
    pdr->container_id = 1;
    pdr->sensor_init = PLDM_NO_INIT;
    pdr->sensor_auxiliary_names_pdr = false;
    pdr->composite_sensor_count = 1;

    auto* possibleStatesPtr = pdr->possible_states;
    auto possibleStates =
        reinterpret_cast<state_sensor_possible_states*>(possibleStatesPtr);
    possibleStates->state_set_id = stateSetID;
    possibleStates->possible_states_size = 2;
    auto state =
        reinterpret_cast<state_sensor_possible_states*>(possibleStates);
    if ((stateSetID == PLDM_OEM_IBM_VERIFICATION_STATE) ||
        (stateSetID == PLDM_OEM_IBM_BOOT_SIDE_RENAME))
        state->states[0].byte = 6;
    else if (stateSetID == PLDM_OEM_IBM_FIRMWARE_UPDATE_STATE)
        state->states[0].byte = 126;
    pldm::responder::pdr_utils::PdrEntry pdrEntry{};
    pdrEntry.data = entry.data();
    pdrEntry.size = pdrSize;
    repo.addRecord(pdrEntry);
}

void buildAllSlotEnableSensorPDR(oem_ibm_platform::Handler* platformHandler,
                                 pdr_utils::Repo& repo,
                                 const std::vector<std::string>& slotobjpaths)
{
    size_t pdrSize = 0;
    pdrSize = sizeof(pldm_state_sensor_pdr) +
              sizeof(state_sensor_possible_states);
    std::vector<uint8_t> entry{};
    entry.resize(pdrSize);
    pldm_state_sensor_pdr* pdr =
        reinterpret_cast<pldm_state_sensor_pdr*>(entry.data());
    if (!pdr)
    {
        error("Failed to get record by PDR type, ERROR:{ERR}", "ERR", lg2::hex,
              static_cast<unsigned>(PLDM_PLATFORM_INVALID_SENSOR_ID));
        return;
    }
    auto& associatedEntityMap = platformHandler->getAssociateEntityMap();
    for (const auto& entity_path : slotobjpaths)
    {
        pdr->hdr.record_handle = 0;
        pdr->hdr.version = 1;
        pdr->hdr.type = PLDM_STATE_SENSOR_PDR;
        pdr->hdr.record_change_num = 0;
        pdr->hdr.length = sizeof(pldm_state_sensor_pdr) - sizeof(pldm_pdr_hdr);
        pdr->terminus_handle = TERMINUS_HANDLE;
        pdr->sensor_id = platformHandler->getNextSensorId();
        if (entity_path != "" && associatedEntityMap.contains(entity_path))
        {
            pdr->entity_type = associatedEntityMap.at(entity_path).entity_type;
            pdr->entity_instance =
                associatedEntityMap.at(entity_path).entity_instance_num;
            pdr->container_id =
                associatedEntityMap.at(entity_path).entity_container_id;
        }
        else
        {
            // the slots are not present, dont create the PDR
            continue;
        }

        pdr->sensor_init = PLDM_NO_INIT;
        pdr->sensor_auxiliary_names_pdr = false;
        pdr->composite_sensor_count = 1;

        auto* possibleStatesPtr = pdr->possible_states;
        auto possibleStates =
            reinterpret_cast<state_sensor_possible_states*>(possibleStatesPtr);
        possibleStates->state_set_id = PLDM_OEM_IBM_PCIE_SLOT_SENSOR_STATE;
        possibleStates->possible_states_size = 1;
        auto state =
            reinterpret_cast<state_sensor_possible_states*>(possibleStates);
        state->states[0].byte = 15;
        pldm::responder::pdr_utils::PdrEntry pdrEntry{};
        pdrEntry.data = entry.data();
        pdrEntry.size = pdrSize;
        repo.addRecord(pdrEntry);
    }
}

void buildAllRealSAIEffecterPDR(oem_ibm_platform::Handler* platformHandler,
                                uint16_t entityType, uint16_t entityInstance,
                                pdr_utils::Repo& repo)
{
    size_t pdrSize = 0;
    pdrSize = sizeof(pldm_state_effecter_pdr) +
              sizeof(state_effecter_possible_states);
    std::vector<uint8_t> entry{};
    entry.resize(pdrSize);
    pldm_state_effecter_pdr* pdr =
        reinterpret_cast<pldm_state_effecter_pdr*>(entry.data());
    if (!pdr)
    {
        error("Failed to get Real SAI effecter PDR record due to the "
              "error {ERR_CODE}",
              "ERR_CODE", lg2::hex,
              static_cast<unsigned>(PLDM_PLATFORM_INVALID_EFFECTER_ID));
        return;
    }
    pdr->hdr.record_handle = 0;
    pdr->hdr.version = 1;
    pdr->hdr.type = PLDM_STATE_EFFECTER_PDR;
    pdr->hdr.record_change_num = 0;
    pdr->hdr.length = sizeof(pldm_state_effecter_pdr) - sizeof(pldm_pdr_hdr);
    pdr->terminus_handle = TERMINUS_HANDLE;
    pdr->effecter_id = platformHandler->getNextEffecterId();
    pdr->entity_type = entityType;
    pdr->entity_instance = entityInstance;
    pdr->container_id = 1;
    pdr->effecter_semantic_id = 0;
    pdr->effecter_init = PLDM_NO_INIT;
    pdr->has_description_pdr = false;
    pdr->composite_effecter_count = 1;

    auto* possibleStatesPtr = pdr->possible_states;
    auto possibleStates =
        reinterpret_cast<state_effecter_possible_states*>(possibleStatesPtr);
    possibleStates->state_set_id = PLDM_STATE_SET_OPERATIONAL_FAULT_STATUS;
    possibleStates->possible_states_size = 1;
    auto state =
        reinterpret_cast<state_effecter_possible_states*>(possibleStates);
    state->states[0].byte = 2;
    pldm::responder::pdr_utils::PdrEntry pdrEntry{};
    pdrEntry.data = entry.data();
    pdrEntry.size = pdrSize;
    repo.addRecord(pdrEntry);
}

void buildAllRealSAISensorPDR(oem_ibm_platform::Handler* platformHandler,
                              uint16_t entityType, uint16_t entityInstance,
                              pdr_utils::Repo& repo)
{
    size_t pdrSize = 0;
    pdrSize = sizeof(pldm_state_sensor_pdr) +
              sizeof(state_sensor_possible_states);
    std::vector<uint8_t> entry{};
    entry.resize(pdrSize);
    pldm_state_sensor_pdr* pdr =
        reinterpret_cast<pldm_state_sensor_pdr*>(entry.data());
    if (!pdr)
    {
        error("Failed to get Real SAI sensor PDR record due to the "
              "error {ERR_CODE}",
              "ERR_CODE", lg2::hex,
              static_cast<unsigned>(PLDM_PLATFORM_INVALID_SENSOR_ID));
        return;
    }
    pdr->hdr.record_handle = 0;
    pdr->hdr.version = 1;
    pdr->hdr.type = PLDM_STATE_SENSOR_PDR;
    pdr->hdr.record_change_num = 0;
    pdr->hdr.length = sizeof(pldm_state_sensor_pdr) - sizeof(pldm_pdr_hdr);
    pdr->terminus_handle = TERMINUS_HANDLE;
    pdr->sensor_id = platformHandler->getNextSensorId();
    pdr->entity_type = entityType;
    pdr->entity_instance = entityInstance;
    pdr->container_id = 1;
    pdr->sensor_init = PLDM_NO_INIT;
    pdr->sensor_auxiliary_names_pdr = false;
    pdr->composite_sensor_count = 1;

    auto* possibleStatesPtr = pdr->possible_states;
    auto possibleStates =
        reinterpret_cast<state_sensor_possible_states*>(possibleStatesPtr);
    possibleStates->state_set_id = PLDM_STATE_SET_OPERATIONAL_FAULT_STATUS;
    possibleStates->possible_states_size = 2;
    auto state =
        reinterpret_cast<state_sensor_possible_states*>(possibleStates);
    state->states[0].byte = 6;
    pldm::responder::pdr_utils::PdrEntry pdrEntry{};
    pdrEntry.data = entry.data();
    pdrEntry.size = pdrSize;
    repo.addRecord(pdrEntry);
}

void buildAllNumericEffecterPDR(oem_ibm_platform::Handler* platformHandler,
                                uint16_t entityType, uint16_t entityInstance,
                                uint16_t effecterSemanticId,
                                pdr_utils::Repo& repo,
                                HostEffecterInstanceMap& instanceMap)
{
    size_t pdrSize = sizeof(pldm_numeric_effecter_value_pdr);
    std::vector<uint8_t> entry{};
    entry.resize(pdrSize);
    pldm_numeric_effecter_value_pdr* pdr =
        reinterpret_cast<pldm_numeric_effecter_value_pdr*>(entry.data());
    if (!pdr)
    {
        error("Failed to get record by PDR type");
        return;
    }

    std::vector<InstanceInfo> info = platformHandler->generateProcAndDcmIDs();

    for (auto procDcmInfo : info)
    {
        pdr->hdr.record_handle = 0;
        pdr->hdr.version = 1;
        pdr->hdr.type = PLDM_NUMERIC_EFFECTER_PDR;
        pdr->hdr.record_change_num = 0;
        pdr->hdr.length = sizeof(pldm_numeric_effecter_value_pdr) -
                          sizeof(pldm_pdr_hdr);
        pdr->terminus_handle = TERMINUS_HANDLE;
        pdr->effecter_id = platformHandler->getNextEffecterId();

        uint16_t effecterId = pdr->effecter_id;

        pdr->entity_type = entityType;

        instanceMap.emplace(effecterId, procDcmInfo);

        entityInstance = procDcmInfo.procId;
        ;

        pdr->entity_instance = entityInstance;
        pdr->container_id = 1; // default
        pdr->effecter_semantic_id = effecterSemanticId;
        pdr->effecter_init = PLDM_NO_INIT;
        pdr->effecter_auxiliary_names = false;
        pdr->base_unit = 0;
        pdr->unit_modifier = 0;
        pdr->rate_unit = 0;
        pdr->base_oem_unit_handle = 0;
        pdr->aux_unit = 0;
        pdr->aux_unit_modifier = 0;
        pdr->aux_oem_unit_handle = 0;
        pdr->aux_rate_unit = 0;
        pdr->is_linear = true;
        pdr->effecter_data_size = PLDM_EFFECTER_DATA_SIZE_UINT32;
        pdr->resolution = 1.00;
        pdr->offset = 0.00;
        pdr->accuracy = 0;
        pdr->plus_tolerance = 0;
        pdr->minus_tolerance = 0;
        pdr->state_transition_interval = 0.00;
        pdr->transition_interval = 0.00;
        pdr->max_settable.value_u32 = 0xFFFFFFFF;
        pdr->min_settable.value_u32 = 0x0;
        pdr->range_field_format = 0;
        pdr->range_field_support.byte = 0;
        pdr->nominal_value.value_u32 = 0;
        pdr->normal_max.value_u32 = 0;
        pdr->normal_min.value_u32 = 0;
        pdr->rated_max.value_u32 = 0;
        pdr->rated_min.value_u32 = 0;

        pldm::responder::pdr_utils::PdrEntry pdrEntry{};
        pdrEntry.data = entry.data();
        pdrEntry.size = pdrSize;
        repo.addRecord(pdrEntry);
    }
}

void buildAllNumericEffecterDimmPDR(oem_ibm_platform::Handler* platformHandler,
                                    uint16_t entityType,
                                    uint16_t entityInstance,
                                    uint16_t effecterSemanticId,
                                    pdr_utils::Repo& repo,
                                    HostEffecterDimmMap& instanceDimmMap)
{
    size_t pdrSize = sizeof(pldm_numeric_effecter_value_pdr);
    std::vector<uint8_t> entry{};
    entry.resize(pdrSize);
    pldm_numeric_effecter_value_pdr* pdr =
        reinterpret_cast<pldm_numeric_effecter_value_pdr*>(entry.data());
    if (!pdr)
    {
        error("Failed to get record by PDR type");
        return;
    }
    if (entityType == PLDM_ENTITY_MEMORY_MODULE &&
        effecterSemanticId == PLDM_OEM_IBM_SBE_SEMANTIC_ID)
    {
        auto dimm_info = platformHandler->generateDimmIds();
        for (auto dimm : dimm_info)
        {
            pdr->hdr.record_handle = 0;
            pdr->hdr.version = 1;
            pdr->hdr.type = PLDM_NUMERIC_EFFECTER_PDR;
            pdr->hdr.record_change_num = 0;
            pdr->hdr.length = sizeof(pldm_numeric_effecter_value_pdr) -
                              sizeof(pldm_pdr_hdr);
            pdr->terminus_handle = TERMINUS_HANDLE;
            pdr->effecter_id = platformHandler->getNextEffecterId();

            uint16_t effecterId = pdr->effecter_id;

            pdr->entity_type = entityType;
            instanceDimmMap.emplace(effecterId, dimm);
            entityInstance = dimm;
            pdr->entity_instance = entityInstance;
            pdr->container_id = 1; // default
            pdr->effecter_semantic_id = effecterSemanticId;
            pdr->effecter_init = PLDM_NO_INIT;
            pdr->effecter_auxiliary_names = false;
            pdr->base_unit = 0;
            pdr->unit_modifier = 0;
            pdr->rate_unit = 0;
            pdr->base_oem_unit_handle = 0;
            pdr->aux_unit = 0;
            pdr->aux_unit_modifier = 0;
            pdr->aux_oem_unit_handle = 0;
            pdr->aux_rate_unit = 0;
            pdr->is_linear = true;
            pdr->effecter_data_size = PLDM_EFFECTER_DATA_SIZE_UINT32;
            pdr->resolution = 1.00;
            pdr->offset = 0.00;
            pdr->accuracy = 0;
            pdr->plus_tolerance = 0;
            pdr->minus_tolerance = 0;
            pdr->state_transition_interval = 0.00;
            pdr->transition_interval = 0.00;
            pdr->max_settable.value_u32 = 0xFFFFFFFF;
            pdr->min_settable.value_u32 = 0x0;
            pdr->range_field_format = 0;
            pdr->range_field_support.byte = 0;
            pdr->nominal_value.value_u32 = 0;
            pdr->normal_max.value_u32 = 0;
            pdr->normal_min.value_u32 = 0;
            pdr->rated_max.value_u32 = 0;
            pdr->rated_min.value_u32 = 0;

            pldm::responder::pdr_utils::PdrEntry pdrEntry{};
            pdrEntry.data = entry.data();
            pdrEntry.size = pdrSize;
            repo.addRecord(pdrEntry);
        }
    }
}

void buildAllDimmSensorPDR(oem_ibm_platform::Handler* platformHandler,
                           uint16_t entityType, uint16_t stateSetID,
                           pdr_utils::Repo& repo)
{
    size_t pdrSize = 0;
    pdrSize = sizeof(pldm_state_sensor_pdr) +
              sizeof(state_sensor_possible_states);
    std::vector<uint8_t> entry{};
    entry.resize(pdrSize);
    pldm_state_sensor_pdr* pdr =
        reinterpret_cast<pldm_state_sensor_pdr*>(entry.data());
    if (!pdr)
    {
        error("Failed to get record by PDR type, ERROR:{ERR}", "ERR", lg2::hex,
              static_cast<unsigned>(PLDM_PLATFORM_INVALID_SENSOR_ID));
        return;
    }
    auto dimm_info = platformHandler->generateDimmIds();
    for (const auto& dimm : dimm_info)
    {
        pdr->hdr.record_handle = 0;
        pdr->hdr.version = 1;
        pdr->hdr.type = PLDM_STATE_SENSOR_PDR;
        pdr->hdr.record_change_num = 0;
        pdr->hdr.length = sizeof(pldm_state_sensor_pdr) - sizeof(pldm_pdr_hdr);
        pdr->terminus_handle = TERMINUS_HANDLE;
        pdr->sensor_id = platformHandler->getNextSensorId();
        pdr->entity_type = entityType;
        pdr->entity_instance = dimm;
        dumpStatusMap[dimm] = DimmDumpState::UNAVAILABLE;
        pdr->container_id = 1;
        pdr->sensor_init = PLDM_NO_INIT;
        pdr->sensor_auxiliary_names_pdr = false;
        pdr->composite_sensor_count = 1;

        auto* possibleStatesPtr = pdr->possible_states;
        auto possibleStates =
            reinterpret_cast<state_sensor_possible_states*>(possibleStatesPtr);
        possibleStates->state_set_id = stateSetID;
        possibleStates->possible_states_size = 1;
        auto state =
            reinterpret_cast<state_sensor_possible_states*>(possibleStates);
        if (stateSetID == PLDM_OEM_IBM_SBE_DUMP_UPDATE_STATE)
            state->states[0].byte = 7;
        pldm::responder::pdr_utils::PdrEntry pdrEntry{};
        pdrEntry.data = entry.data();
        pdrEntry.size = pdrSize;
        repo.addRecord(pdrEntry);
    }
}

void pldm::responder::oem_ibm_platform::Handler::buildOEMPDR(
    pdr_utils::Repo& repo)
{
    buildAllCodeUpdateEffecterPDR(this, PLDM_OEM_IBM_ENTITY_FIRMWARE_UPDATE,
                                  ENTITY_INSTANCE_0,
                                  PLDM_OEM_IBM_FIRMWARE_UPDATE_STATE, repo);
    buildAllCodeUpdateEffecterPDR(this, PLDM_ENTITY_SYSTEM_CHASSIS,
                                  ENTITY_INSTANCE_1,
                                  PLDM_OEM_IBM_SYSTEM_POWER_STATE, repo);

    static constexpr auto objectPath = "/xyz/openbmc_project/inventory/system";
    const std::vector<std::string> slotInterface = {
        "xyz.openbmc_project.Inventory.Item.PCIeSlot"};
    auto slotPaths = dBusIntf->getSubTreePaths(objectPath, 0, slotInterface);

    buildAllSlotEnableEffecterPDR(this, repo, slotPaths);
    buildAllSlotEnableSensorPDR(this, repo, slotPaths);

    buildAllRealSAIEffecterPDR(this, PLDM_OEM_IBM_ENTITY_REAL_SAI,
                               ENTITY_INSTANCE_1, repo);
    buildAllRealSAISensorPDR(this, PLDM_OEM_IBM_ENTITY_REAL_SAI,
                             ENTITY_INSTANCE_1, repo);
    buildAllCodeUpdateEffecterPDR(this, PLDM_OEM_IBM_ENTITY_FIRMWARE_UPDATE,
                                  ENTITY_INSTANCE_0,
                                  PLDM_OEM_IBM_BOOT_SIDE_RENAME, repo);

    buildAllCodeUpdateSensorPDR(this, PLDM_OEM_IBM_ENTITY_FIRMWARE_UPDATE,
                                ENTITY_INSTANCE_0,
                                PLDM_OEM_IBM_FIRMWARE_UPDATE_STATE, repo);
    buildAllCodeUpdateSensorPDR(this, PLDM_OEM_IBM_ENTITY_FIRMWARE_UPDATE,
                                ENTITY_INSTANCE_0,
                                PLDM_OEM_IBM_VERIFICATION_STATE, repo);
    buildAllCodeUpdateSensorPDR(this, PLDM_OEM_IBM_ENTITY_FIRMWARE_UPDATE,
                                ENTITY_INSTANCE_0,
                                PLDM_OEM_IBM_BOOT_SIDE_RENAME, repo);

    buildAllSystemPowerStateEffecterPDR(
        this, PLDM_OEM_IBM_CHASSIS_POWER_CONTROLLER, ENTITY_INSTANCE_0,
        PLDM_STATE_SET_SYSTEM_POWER_STATE, repo);

    pldm_entity fwUpEntity = {PLDM_OEM_IBM_ENTITY_FIRMWARE_UPDATE, 0, 1};
    attachOemEntityToEntityAssociationPDR(
        this, bmcEntityTree, "/xyz/openbmc_project/inventory/system", repo,
        fwUpEntity);
    pldm_entity saiEntity = {PLDM_OEM_IBM_ENTITY_REAL_SAI, 1, 1};
    attachOemEntityToEntityAssociationPDR(
        this, bmcEntityTree, "/xyz/openbmc_project/inventory/system", repo,
        saiEntity);

    pldm_entity powerStateEntity = {PLDM_OEM_IBM_CHASSIS_POWER_CONTROLLER, 0,
                                    1};
    attachOemEntityToEntityAssociationPDR(
        this, bmcEntityTree, "/xyz/openbmc_project/inventory/system", repo,
        powerStateEntity);

    realSAISensorId = findStateSensorId(
        repo.getPdr(), 0, PLDM_OEM_IBM_ENTITY_REAL_SAI, ENTITY_INSTANCE_1, 1,
        PLDM_STATE_SET_OPERATIONAL_FAULT_STATUS);

    buildAllNumericEffecterPDR(this, PLDM_ENTITY_PROC, ENTITY_INSTANCE_0,
                               PLDM_OEM_IBM_SBE_SEMANTIC_ID, repo, instanceMap);
    buildAllNumericEffecterDimmPDR(
        this, PLDM_ENTITY_MEMORY_MODULE, ENTITY_INSTANCE_0,
        PLDM_OEM_IBM_SBE_SEMANTIC_ID, repo, instanceDimmMap);
    buildAllDimmSensorPDR(this, PLDM_ENTITY_MEMORY_MODULE,
                          PLDM_OEM_IBM_SBE_DUMP_UPDATE_STATE, repo);
    auto sensorId = findStateSensorId(
        repo.getPdr(), 0, PLDM_OEM_IBM_ENTITY_FIRMWARE_UPDATE,
        ENTITY_INSTANCE_0, 1, PLDM_OEM_IBM_VERIFICATION_STATE);
    codeUpdate->setMarkerLidSensor(sensorId);
    sensorId = findStateSensorId(
        repo.getPdr(), 0, PLDM_OEM_IBM_ENTITY_FIRMWARE_UPDATE,
        ENTITY_INSTANCE_0, 1, PLDM_OEM_IBM_FIRMWARE_UPDATE_STATE);
    codeUpdate->setFirmwareUpdateSensor(sensorId);
    sensorId =
        findStateSensorId(repo.getPdr(), 0, PLDM_OEM_IBM_ENTITY_FIRMWARE_UPDATE,
                          ENTITY_INSTANCE_0, 1, PLDM_OEM_IBM_BOOT_SIDE_RENAME);
    codeUpdate->setBootSideRenameStateSensor(sensorId);
}

void pldm::responder::oem_ibm_platform::Handler::setPlatformHandler(
    pldm::responder::platform::Handler* handler)
{
    platformHandler = handler;
}

int pldm::responder::oem_ibm_platform::Handler::sendEventToHost(
    std::vector<uint8_t>& requestMsg, uint8_t instanceId)
{
    if (requestMsg.size())
    {
        std::ostringstream tempStream;
        for (int byte : requestMsg)
        {
            tempStream << std::setfill('0') << std::setw(2) << std::hex << byte
                       << " ";
        }
        std::cout << tempStream.str() << std::endl;
    }
    auto oemPlatformEventMessageResponseHandler =
        [](mctp_eid_t /*eid*/, const pldm_msg* response, size_t respMsgLen) {
        uint8_t completionCode{};
        uint8_t status{};
        auto rc = decode_platform_event_message_resp(response, respMsgLen,
                                                     &completionCode, &status);
        if (rc || completionCode)
        {
            error(
                "Failed to decode platform event message response for code update event with response code '{RC}' and completion code '{CC}'",
                "RC", rc, "CC", static_cast<unsigned>(completionCode));
        }
    };
    auto rc = handler->registerRequest(
        mctp_eid, instanceId, PLDM_PLATFORM, PLDM_PLATFORM_EVENT_MESSAGE,
        std::move(requestMsg),
        std::move(oemPlatformEventMessageResponseHandler));
    if (rc)
    {
        error("Failed to send BIOS attribute change event message ");
    }

    return rc;
}

int encodeEventMsg(uint8_t eventType, const std::vector<uint8_t>& eventDataVec,
                   std::vector<uint8_t>& requestMsg, uint8_t instanceId)
{
    auto request = reinterpret_cast<pldm_msg*>(requestMsg.data());

    auto rc = encode_platform_event_message_req(
        instanceId, 1 /*formatVersion*/, TERMINUS_ID /*tId*/, eventType,
        eventDataVec.data(), eventDataVec.size(), request,
        eventDataVec.size() + PLDM_PLATFORM_EVENT_MESSAGE_MIN_REQ_BYTES);

    return rc;
}

void pldm::responder::oem_ibm_platform::Handler::setDimmStateSensor(
    bool status, uint16_t entityInstance)
{
    auto pdrs = findStateSensorPDR(TERMINUS_ID, PLDM_ENTITY_MEMORY_MODULE,
                                   PLDM_OEM_IBM_SBE_DUMP_UPDATE_STATE, pdrRepo);
    if (pdrs.empty())
    {
        error(
            "Failed to find state sensor PDR of entity type 'PLDM_ENTITY_MEMORY_MODULE' and state set id 'PLDM_OEM_IBM_SBE_DUMP_UPDATE_STATE' for entity instance = {ENT_INSTANCE}",
            "ENT_INSTANCE", entityInstance);
        return;
    }
    for (auto& pdr : pdrs)
    {
        auto stateSensorPDR =
            reinterpret_cast<pldm_state_sensor_pdr*>(pdr.data());
        if (entityInstance == stateSensorPDR->entity_instance)
        {
            auto sid = stateSensorPDR->sensor_id;
            info(
                "Sending state sensor event for sensor ID {SID} with status {STATUS}",
                "SID", sid, "STATUS", status);
            if (status)
            {
                sendStateSensorEvent(stateSensorPDR->sensor_id,
                                     PLDM_STATE_SENSOR_STATE, 0,
                                     uint8_t(DimmDumpState::SUCCESS),
                                     uint8_t(dumpStatusMap[entityInstance]));
                dumpStatusMap[entityInstance] = DimmDumpState::SUCCESS;
            }
            else
            {
                sendStateSensorEvent(stateSensorPDR->sensor_id,
                                     PLDM_STATE_SENSOR_STATE, 0,
                                     uint8_t(DimmDumpState::RETRY),
                                     uint8_t(dumpStatusMap[entityInstance]));
                dumpStatusMap[entityInstance] = DimmDumpState::RETRY;
            }
        }
    }
}

int pldm::responder::oem_ibm_platform::Handler::fetchDimmStateSensor(
    uint16_t entityInstance)
{
    return dumpStatusMap[entityInstance];
}

void pldm::responder::oem_ibm_platform::Handler::setHostEffecterState(
    bool status, uint16_t entityTypeReceived, uint16_t entityInstance)
{
    pldm::pdr::EntityType entityType;
    if (entityTypeReceived == PLDM_ENTITY_PROC)
    {
        entityType = PLDM_ENTITY_PROC;
    }
    else
    {
        error("Invalid entity type received: {ENTITY_TYPE}", "ENTITY_TYPE",
              entityTypeReceived);
        return;
    }

    auto pdrs = findStateEffecterPDR(
        TERMINUS_ID, entityType, PLDM_OEM_IBM_SBE_MAINTENANCE_STATE, pdrRepo);
    for (auto& pdr : pdrs)
    {
        auto stateEffecterPDR =
            reinterpret_cast<pldm_state_effecter_pdr*>(pdr.data());
        uint16_t effecterId = stateEffecterPDR->effecter_id;
        if (entityInstance == stateEffecterPDR->entity_instance)
        {
            uint8_t compEffecterCount =
                stateEffecterPDR->composite_effecter_count;

            std::vector<uint8_t> requestMsg(
                sizeof(pldm_msg_hdr) + sizeof(effecterId) +
                    sizeof(compEffecterCount) +
                    sizeof(set_effecter_state_field) * compEffecterCount,
                0);

            auto instanceId = instanceIdDb.next(mctp_eid);

            auto request = reinterpret_cast<pldm_msg*>(requestMsg.data());
            std::vector<set_effecter_state_field> stateField;
            info(
                "State effecter ID {EFFECTER_ID} will be set with state as - {STATUS}",
                "EFFECTER_ID", effecterId, "STATUS", status);
            if (status == true)
            {
                stateField.push_back(set_effecter_state_field{
                    PLDM_REQUEST_SET, SBE_DUMP_COMPLETED});
            }
            else
            {
                stateField.push_back(set_effecter_state_field{
                    PLDM_REQUEST_SET, SBE_RETRY_REQUIRED});
            }
            auto rc = encode_set_state_effecter_states_req(
                instanceId, effecterId, compEffecterCount, stateField.data(),
                request);
            if (rc != PLDM_SUCCESS)
            {
                error(
                    "Set state effecter state command for {EFF_ID} failure. PLDM error code: {RESPONSE_CODE}",
                    "EFF_ID", effecterId, "RESPONSE_CODE", rc);
                instanceIdDb.free(mctp_eid, instanceId);
                return;
            }
            auto setStateEffecterStatesRespHandler =
                [=, this](mctp_eid_t /*eid*/, const pldm_msg* response,
                          size_t respMsgLen) {
                if (response == nullptr || !respMsgLen)
                {
                    error(
                        "Failed to receive response for setstateEffecterSates command for {EFF_ID}",
                        "EFF_ID", effecterId);
                    return;
                }
                uint8_t completionCode{};
                auto rc = decode_set_state_effecter_states_resp(
                    response, respMsgLen, &completionCode);
                if (rc)
                {
                    error(
                        "Failed to decode setStateEffecterStates for {EFF_ID} response: {RESPONSE_CODE}",
                        "EFF_ID", effecterId, "RESPONSE_CODE", rc);
                    pldm::utils::reportError(
                        "xyz.openbmc_project.PLDM.Error.SetHostEffecterFailed");
                }
                if (completionCode)
                {
                    error(
                        "Failed to set a Host effecter for {EFF_ID}: {COMPLETION_CODE}",
                        "EFF_ID", effecterId, "COMPLETION_CODE",
                        completionCode);
                    pldm::utils::reportError(
                        "xyz.openbmc_project.PLDM.Error.SetHostEffecterFailed");
                }
            };
            rc = handler->registerRequest(
                mctp_eid, instanceId, PLDM_PLATFORM,
                PLDM_SET_STATE_EFFECTER_STATES, std::move(requestMsg),
                std::move(setStateEffecterStatesRespHandler));
            if (rc)
            {
                error(
                    "Failed to send request to set an effecter {EFF_ID} on Host",
                    "EFF_ID", effecterId);
            }
        }
    }
}

void pldm::responder::oem_ibm_platform::Handler::monitorDump(
    const std::string& obj_path, uint16_t entityType, uint16_t entityInstance)
{
    std::string matchInterface = "xyz.openbmc_project.Common.Progress";
    sbeDumpMatch = std::make_unique<sdbusplus::bus::match_t>(
        pldm::utils::DBusHandler::getBus(),
        sdbusplus::bus::match::rules::propertiesChanged(obj_path.c_str(),
                                                        matchInterface.c_str()),
        [=, this](sdbusplus::message_t& msg) {
        DbusChangedProps props{};
        std::string intf;
        msg.read(intf, props);
        const auto itr = props.find("Status");
        if (itr != props.end())
        {
            PropertyValue value = itr->second;
            auto propVal = std::get<std::string>(value);
            if (propVal ==
                "xyz.openbmc_project.Common.Progress.OperationStatus.Completed")
            {
                if (entityType == PLDM_ENTITY_MEMORY_MODULE)
                {
                    setDimmStateSensor(true, entityInstance);
                }
                else
                {
                    setHostEffecterState(true, entityType, entityInstance);
                }
            }
            else if (
                propVal ==
                    "xyz.openbmc_project.Common.Progress.OperationStatus.Failed" ||
                propVal ==
                    "xyz.openbmc_project.Common.Progress.OperationStatus.Aborted")
            {
                if (entityType == PLDM_ENTITY_MEMORY_MODULE)
                {
                    setDimmStateSensor(false, entityInstance);
                }
                else
                {
                    setHostEffecterState(false, entityType, entityInstance);
                }
            }
        }
        sbeDumpMatch = nullptr;
    });
}

int pldm::responder::oem_ibm_platform::Handler::setNumericEffecter(
    uint16_t entityInstance, const PropertyValue& propertyValue,
    uint16_t entityType)
{
    static constexpr auto objectPath = "/xyz/openbmc_project/dump/system";
    static constexpr auto interface = "xyz.openbmc_project.Dump.Create";

    uint32_t value = std::get<uint32_t>(propertyValue);
    auto& bus = pldm::utils::DBusHandler::getBus();

    try
    {
        auto service = pldm::utils::DBusHandler().getService(objectPath,
                                                             interface);
        auto method = bus.new_method_call(service.c_str(), objectPath,
                                          interface, "CreateDump");
        static constexpr auto dumpParam =
            "com.ibm.Dump.Create.CreateParameters.DumpType";
        static constexpr auto errLogIdParam =
            "com.ibm.Dump.Create.CreateParameters.ErrorLogId";
        static constexpr auto failingUnitIdParam =
            "com.ibm.Dump.Create.CreateParameters.FailingUnitId";
        static constexpr auto sbeParam = "com.ibm.Dump.Create.DumpType.SBE";
        static constexpr auto msbeParam =
            "com.ibm.Dump.Create.DumpType.MemoryBufferSBE";

        std::map<std::string, std::variant<std::string, uint64_t>> createParams;
        if (entityType == PLDM_ENTITY_MEMORY_MODULE)
        {
            createParams[dumpParam] = msbeParam;
        }
        else if (entityType == PLDM_ENTITY_PROC)
        {
            createParams[dumpParam] = sbeParam;
        }
        else
        {
            error("Invalid entity type received: {ENTITY_TYPE}", "ENTITY_TYPE",
                  entityType);
            return PLDM_ERROR;
        }
        createParams[errLogIdParam] = (uint64_t)value;
        createParams[failingUnitIdParam] = (uint64_t)entityInstance;
        method.append(createParams);

        auto response = bus.call(method);

        sdbusplus::message::object_path reply;
        response.read(reply);
        info(
            "Setting numeric effecter of type {TYPE} with entity instance {INST}",
            "TYPE", entityType, "INST", entityInstance);
        monitorDump(reply, entityType, entityInstance);
    }
    catch (const std::exception& e)
    {
        error(
            "Failed to make a DBus call as the dump policy is disabled, ERROR= {ERR}",
            "ERR", e);
        // case when the dump policy is disabled but we set the host effecter as
        // true and the host moves on
        if (entityType == PLDM_ENTITY_MEMORY_MODULE)
        {
            setDimmStateSensor(true, entityInstance);
        }
        else
        {
            setHostEffecterState(true, entityType, entityInstance);
        }
    }
    return PLDM_SUCCESS;
}

std::vector<std::string>
    pldm::responder::oem_ibm_platform::Handler::getProcObjectPaths()
{
    static constexpr auto searchpath = "/xyz/openbmc_project/inventory/system";
    int depth = 0;
    std::vector<std::string> procInterface = {
        "xyz.openbmc_project.Inventory.Item.Cpu"};
    pldm::utils::GetSubTreeResponse response =
        dBusIntf->getSubtree(searchpath, depth, procInterface);
    std::vector<std::string> procPaths;
    for (const auto& [objPath, serviceMap] : response)
    {
        procPaths.emplace_back(objPath);
    }
    return procPaths;
}

std::vector<std::string>
    pldm::responder::oem_ibm_platform::Handler::getDimmObjectPaths()
{
    static constexpr auto searchpath = "/xyz/openbmc_project/inventory/system";
    static constexpr auto itemPath = "xyz.openbmc_project.Inventory.Item.Dimm";
    int depth = 0;
    std::vector<std::string> dimmInterface = {itemPath};
    pldm::utils::GetSubTreeResponse response =
        dBusIntf->getSubtree(searchpath, depth, dimmInterface);
    std::vector<std::string> dimmPaths;
    for (const auto& [objPath, serviceMap] : response)
    {
        dimmPaths.emplace_back(objPath);
    }
    return dimmPaths;
}

void pldm::responder::oem_ibm_platform::Handler::sendStateSensorEvent(
    uint16_t sensorId, enum sensor_event_class_states sensorEventClass,
    uint8_t sensorOffset, uint8_t eventState, uint8_t prevEventState)
{
    std::vector<uint8_t> sensorEventDataVec{};
    size_t sensorEventSize = PLDM_SENSOR_EVENT_DATA_MIN_LENGTH + 1;
    sensorEventDataVec.resize(sensorEventSize);
    auto eventData = reinterpret_cast<struct pldm_sensor_event_data*>(
        sensorEventDataVec.data());
    eventData->sensor_id = sensorId;
    eventData->sensor_event_class_type = sensorEventClass;
    auto eventClassStart = eventData->event_class;
    auto eventClass =
        reinterpret_cast<struct pldm_sensor_event_state_sensor_state*>(
            eventClassStart);
    eventClass->sensor_offset = sensorOffset;
    eventClass->event_state = eventState;
    eventClass->previous_event_state = prevEventState;
    auto instanceId = instanceIdDb.next(mctp_eid);
    std::vector<uint8_t> requestMsg(sizeof(pldm_msg_hdr) +
                                    PLDM_PLATFORM_EVENT_MESSAGE_MIN_REQ_BYTES +
                                    sensorEventDataVec.size());
    auto rc = encodeEventMsg(PLDM_SENSOR_EVENT, sensorEventDataVec, requestMsg,
                             instanceId);
    if (rc != PLDM_SUCCESS)
    {
        error("Failed to encode state sensor event with response code '{RC}'",
              "RC", rc);
        instanceIdDb.free(mctp_eid, instanceId);
        return;
    }
    rc = sendEventToHost(requestMsg, instanceId);
    if (rc != PLDM_SUCCESS)
    {
        error(
            "Failed to send event to remote terminus with response code '{RC}'",
            "RC", rc);
    }
    return;
}

void pldm::responder::oem_ibm_platform::Handler::_processEndUpdate(
    sdeventplus::source::EventBase& /*source */)
{
    assembleImageEvent.reset();
    info("Starting assembleCodeUpdateImage");
    int retc = codeUpdate->assembleCodeUpdateImage();
    if (retc != PLDM_SUCCESS)
    {
        codeUpdate->setCodeUpdateProgress(false);
        auto sensorId = codeUpdate->getFirmwareUpdateSensor();
        sendStateSensorEvent(sensorId, PLDM_STATE_SENSOR_STATE, 0,
                             uint8_t(CodeUpdateState::FAIL),
                             uint8_t(CodeUpdateState::START));
    }
}

void pldm::responder::oem_ibm_platform::Handler::_processStartUpdate(
    sdeventplus::source::EventBase& /*source */)
{
    codeUpdate->deleteImage();
    CodeUpdateState state = CodeUpdateState::START;
    auto rc = codeUpdate->setRequestedApplyTime();
    if (rc != PLDM_SUCCESS)
    {
        error("setRequestedApplyTime failed");
        state = CodeUpdateState::FAIL;
    }
    auto sensorId = codeUpdate->getFirmwareUpdateSensor();
    info("Sending Start Update sensor event to PHYP");
    sendStateSensorEvent(sensorId, PLDM_STATE_SENSOR_STATE, 0, uint8_t(state),
                         uint8_t(CodeUpdateState::END));
}

void pldm::responder::oem_ibm_platform::Handler::updateOemDbusPaths(
    std::string& dbusPath)
{
    std::string toFind("system1/chassis1/motherboard1");
    if (dbusPath.find(toFind) != std::string::npos)
    {
        size_t pos = dbusPath.find(toFind);
        dbusPath.replace(pos, toFind.length(), "system/chassis/motherboard");
    }
    toFind = "system1";
    if (dbusPath.find(toFind) != std::string::npos)
    {
        size_t pos = dbusPath.find(toFind);
        dbusPath.replace(pos, toFind.length(), "system");
    }
    /* below logic to replace path 'motherboard/socket/chassis' to
       'motherboard/chassis' or 'motherboard/socket123/chassis' to
       'motherboard/chassis' */
    toFind = "socket";
    if (dbusPath.find(toFind) != std::string::npos)
    {
        std::regex reg(R"(\/motherboard\/socket[0-9]+)");
        dbusPath = regex_replace(dbusPath, reg, "/motherboard");
    }
}

void pldm::responder::oem_ibm_platform::Handler::_processSystemReboot(
    sdeventplus::source::EventBase& /*source */)
{
    BiosAttributeList biosAttrList;
    biosAttrList.push_back(std::make_pair("pvm_boot_initiator", "Host"));
    setBiosAttr(biosAttrList);
    pldm::utils::PropertyValue value =
        "xyz.openbmc_project.State.Chassis.Transition.Off";
    pldm::utils::DBusMapping dbusMapping{"/xyz/openbmc_project/state/chassis0",
                                         "xyz.openbmc_project.State.Chassis",
                                         "RequestedPowerTransition", "string"};
    try
    {
        info(
            "InbandCodeUpdate: ChassisOff the host. Current Boot Side {C}, Next boot Side {N}",
            "C", getBiosAttrValue("fw_boot_side_current"), "N",
            getBiosAttrValue("fw_boot_side"));
        dBusIntf->setDbusProperty(dbusMapping, value);
    }
    catch (const std::exception& e)
    {
        error(
            "Failure in chassis State transition to Off, unable to set property RequestedPowerTransition, error - {ERROR}",
            "ERROR", e);
    }

    using namespace sdbusplus::bus::match::rules;
    chassisOffMatch = std::make_unique<sdbusplus::bus::match_t>(
        pldm::utils::DBusHandler::getBus(),
        propertiesChanged("/xyz/openbmc_project/state/chassis0",
                          "xyz.openbmc_project.State.Chassis"),
        [this](sdbusplus::message_t& msg) {
        DbusChangedProps props{};
        std::string intf;
        msg.read(intf, props);
        const auto itr = props.find("CurrentPowerState");
        if (itr != props.end())
        {
            PropertyValue value = itr->second;
            auto propVal = std::get<std::string>(value);
            if (propVal == "xyz.openbmc_project.State.Chassis.PowerState.Off")
            {
                pldm::utils::DBusMapping dbusMapping{
                    "/xyz/openbmc_project/control/host0/"
                    "power_restore_policy/one_time",
                    "xyz.openbmc_project.Control.Power.RestorePolicy",
                    "PowerRestorePolicy", "string"};
                value = "xyz.openbmc_project.Control.Power.RestorePolicy."
                        "Policy.AlwaysOn";
                try
                {
                    info("InbandCodeUpdate: Setting the one time APR policy");
                    dBusIntf->setDbusProperty(dbusMapping, value);
                }
                catch (const std::exception& e)
                {
                    error(
                        "Failure in setting one-time restore policy, unable to set property PowerRestorePolicy, error - {ERROR}",
                        "ERROR", e);
                }
                dbusMapping = pldm::utils::DBusMapping{
                    "/xyz/openbmc_project/state/bmc0",
                    "xyz.openbmc_project.State.BMC", "RequestedBMCTransition",
                    "string"};
                value = "xyz.openbmc_project.State.BMC.Transition.Reboot";
                try
                {
                    info("InbandCodeUpdate: Rebooting the BMC");
                    dBusIntf->setDbusProperty(dbusMapping, value);
                }
                catch (const std::exception& e)
                {
                    error(
                        "Failure in BMC state transition to reboot, unable to set property RequestedBMCTransition , error - {ERROR}",
                        "ERROR", e);
                }
            }
        }
    });
}

void pldm::responder::oem_ibm_platform::Handler::checkAndDisableWatchDog()
{
    if (!hostOff && setEventReceiverCnt == SET_EVENT_RECEIVER_SENT)
    {
        disableWatchDogTimer();
    }

    return;
}

bool pldm::responder::oem_ibm_platform::Handler::watchDogRunning()
{
    static constexpr auto watchDogObjectPath =
        "/xyz/openbmc_project/watchdog/host0";
    static constexpr auto watchDogEnablePropName = "Enabled";
    static constexpr auto watchDogInterface =
        "xyz.openbmc_project.State.Watchdog";
    bool isWatchDogRunning = false;
    try
    {
        isWatchDogRunning = pldm::utils::DBusHandler().getDbusProperty<bool>(
            watchDogObjectPath, watchDogEnablePropName, watchDogInterface);
    }
    catch (const std::exception&)
    {
        return false;
    }
    return isWatchDogRunning;
}

void pldm::responder::oem_ibm_platform::Handler::resetWatchDogTimer()
{
    static constexpr auto watchDogService = "xyz.openbmc_project.Watchdog";
    static constexpr auto watchDogObjectPath =
        "/xyz/openbmc_project/watchdog/host0";
    static constexpr auto watchDogInterface =
        "xyz.openbmc_project.State.Watchdog";
    static constexpr auto watchDogResetPropName = "ResetTimeRemaining";

    bool wdStatus = watchDogRunning();
    if (wdStatus == false)
    {
        return;
    }
    try
    {
        auto& bus = pldm::utils::DBusHandler::getBus();
        auto resetMethod =
            bus.new_method_call(watchDogService, watchDogObjectPath,
                                watchDogInterface, watchDogResetPropName);
        resetMethod.append(true);
        bus.call_noreply(resetMethod, dbusTimeout);
    }
    catch (const std::exception& e)
    {
        error("Failed to reset watchdog timer, error - {ERROR}", "ERROR", e);
        return;
    }
}

void pldm::responder::oem_ibm_platform::Handler::disableWatchDogTimer()
{
    setEventReceiverCnt = 0;
    pldm::utils::DBusMapping dbusMapping{"/xyz/openbmc_project/watchdog/host0",
                                         "xyz.openbmc_project.State.Watchdog",
                                         "Enabled", "bool"};
    bool wdStatus = watchDogRunning();

    if (!wdStatus)
    {
        return;
    }
    try
    {
        pldm::utils::DBusHandler().setDbusProperty(dbusMapping, false);
    }
    catch (const std::exception& e)
    {
        error("Failed to disable watchdog timer, error - {ERROR}", "ERROR", e);
    }
}
int pldm::responder::oem_ibm_platform::Handler::checkBMCState()
{
    using BMC = sdbusplus::client::xyz::openbmc_project::state::BMC<>;
    auto bmcPath = sdbusplus::message::object_path(BMC::namespace_path::value) /
                   BMC::namespace_path::bmc;
    try
    {
        pldm::utils::PropertyValue propertyValue =
            pldm::utils::DBusHandler().getDbusPropertyVariant(
                bmcPath.str.c_str(), "CurrentBMCState", BMC::interface);

        if (std::get<std::string>(propertyValue) ==
            "xyz.openbmc_project.State.BMC.BMCState.NotReady")
        {
            error("GetPDR : PLDM stack is not ready for PDR exchange");
            return PLDM_ERROR_NOT_READY;
        }
    }
    catch (const std::exception& e)
    {
        error("Error getting the current BMC state, error - {ERROR}", "ERROR",
              e);
        return PLDM_ERROR;
    }
    return PLDM_SUCCESS;
}

void pldm::responder::oem_ibm_platform::Handler::updateContainerID()
{
    for (auto& [key, value] : instanceMap)
    {
        uint16_t newContainerID = pldm_find_container_id(
            pdrRepo, PLDM_ENTITY_PROC_MODULE, value.dcmId, HOST_PDR_START_RANGE,
            HOST_PDR_END_RANGE);
        pldm_change_container_id_of_effecter(pdrRepo, key, newContainerID);
    }
    for (auto& [key, value] : instanceDimmMap)
    {
        uint16_t newDimmContainerID =
            pldm_find_container_id(pdrRepo, PLDM_ENTITY_MEMORY_BOARD, value,
                                   HOST_PDR_START_RANGE, HOST_PDR_END_RANGE);
        pldm_change_container_id_of_effecter(pdrRepo, key, newDimmContainerID);
    }
}

const pldm_pdr_record*
    pldm::responder::oem_ibm_platform::Handler::fetchLastBMCRecord(
        const pldm_pdr* repo)
{
    return pldm_pdr_find_last_in_range(repo, BMC_PDR_START_RANGE,
                                       BMC_PDR_END_RANGE);
}

bool pldm::responder::oem_ibm_platform::Handler::checkRecordHandleInRange(
    const uint32_t& record_handle)
{
    return record_handle >= HOST_PDR_START_RANGE &&
           record_handle <= HOST_PDR_END_RANGE;
}

void Handler::processSetEventReceiver()
{
    this->setEventReceiver();
}

void pldm::responder::oem_ibm_platform::Handler::
    processPowerCycleOffSoftGraceful()
{
    error("Received soft graceful power cycle request");
    pldm::utils::PropertyValue value =
        "xyz.openbmc_project.State.Host.Transition.ForceWarmReboot";
    pldm::utils::DBusMapping dbusMapping{"/xyz/openbmc_project/state/host0",
                                         "xyz.openbmc_project.State.Host",
                                         "RequestedHostTransition", "string"};
    try
    {
        dBusIntf->setDbusProperty(dbusMapping, value);
    }
    catch (const std::exception& e)
    {
        error(
            "Error to do a ForceWarmReboot, chassis power remains on, and boot the host back up. Unable to set property RequestedHostTransition. ERROR={ERR_EXCEP}",
            "ERR_EXCEP", e);
    }
}

void pldm::responder::oem_ibm_platform::Handler::processPowerOffSoftGraceful()
{
    error("Received soft power off graceful request");
    pldm::utils::PropertyValue value =
        "xyz.openbmc_project.State.Chassis.Transition.Off";
    pldm::utils::DBusMapping dbusMapping{"/xyz/openbmc_project/state/chassis0",
                                         "xyz.openbmc_project.State.Chassis",
                                         "RequestedPowerTransition", "string"};
    try
    {
        dBusIntf->setDbusProperty(dbusMapping, value);
    }
    catch (const std::exception& e)
    {
        error(
            "Error in powering down the host. Unable to set property RequestedPowerTransition. ERROR={ERR_EXCEP}",
            "ERR_EXCEP", e);
    }
}

void pldm::responder::oem_ibm_platform::Handler::processPowerOffHardGraceful()
{
    error("Received hard power off graceful request");
    pldm::utils::PropertyValue value =
        "xyz.openbmc_project.Control.Power.RestorePolicy.Policy.AlwaysOn";
    pldm::utils::DBusMapping dbusMapping{
        "/xyz/openbmc_project/control/host0/power_restore_policy/one_time",
        "xyz.openbmc_project.Control.Power.RestorePolicy", "PowerRestorePolicy",
        "string"};
    try
    {
        auto customerPolicy =
            pldm::utils::DBusHandler().getDbusProperty<std::string>(
                "/xyz/openbmc_project/control/host0/power_restore_policy",
                "PowerRestorePolicy",
                "xyz.openbmc_project.Control.Power.RestorePolicy");
        if (customerPolicy !=
            "xyz.openbmc_project.Control.Power.RestorePolicy.Policy.AlwaysOff")
        {
            dBusIntf->setDbusProperty(dbusMapping, value);
        }
    }
    catch (const std::exception& e)
    {
        error(
            "Setting one-time restore policy failed, Unable to set property PowerRestorePolicy. ERROR={ERR_EXCEP}",
            "ERR_EXCEP", e);
    }
    processPowerOffSoftGraceful();
}

void pldm::responder::oem_ibm_platform::Handler::turnOffRealSAIEffecter()
{
    try
    {
        pldm::utils::DBusMapping dbusPartitionMapping{
            "/xyz/openbmc_project/led/groups/partition_system_attention_indicator",
            "xyz.openbmc_project.Led.Group", "Asserted", "bool"};
        pldm::utils::DBusHandler().setDbusProperty(dbusPartitionMapping, false);
    }
    catch (const std::exception& e)
    {
        error("Turn off of partition SAI effecter failed with "
              "error:{ERR_EXCEP}",
              "ERR_EXCEP", e);
    }
    try
    {
        pldm::utils::DBusMapping dbusPlatformMapping{
            "/xyz/openbmc_project/led/groups/platform_system_attention_indicator",
            "xyz.openbmc_project.Led.Group", "Asserted", "bool"};
        pldm::utils::DBusHandler().setDbusProperty(dbusPlatformMapping, false);
    }
    catch (const std::exception& e)
    {
        error("Turn off of platform SAI effecter failed with "
              "error:{ERR_EXCEP}",
              "ERR_EXCEP", e);
    }
}

uint8_t pldm::responder::oem_ibm_platform::Handler::fetchRealSAIStatus()
{
    try
    {
        auto isPartitionSAIOn = pldm::utils::DBusHandler().getDbusProperty<bool>(
            "/xyz/openbmc_project/led/groups/partition_system_attention_indicator",
            "Asserted", "xyz.openbmc_project.Led.Group");
        auto isPlatformSAIOn = pldm::utils::DBusHandler().getDbusProperty<bool>(
            "/xyz/openbmc_project/led/groups/platform_system_attention_indicator",
            "Asserted", "xyz.openbmc_project.Led.Group");

        if (isPartitionSAIOn || isPlatformSAIOn)
        {
            return PLDM_SENSOR_WARNING;
        }
    }
    catch (const std::exception& e)
    {
        error("Fetching of Real SAI sensor status failed with "
              "error:{ERR_EXCEP}",
              "ERR_EXCEP", e);
    }
    return PLDM_SENSOR_NORMAL;
}

void pldm::responder::oem_ibm_platform::Handler::processSAIUpdate()
{
    auto realSAIState = fetchRealSAIStatus();
    sendStateSensorEvent(realSAISensorId, PLDM_STATE_SENSOR_STATE, 0,
                         uint8_t(realSAIState), uint8_t(PLDM_SENSOR_UNKNOWN));
}

void pldm::responder::oem_ibm_platform::Handler::setBitmapMethodCall(
    const std::string& objPath, const std::string& dbusMethod,
    const std::string& dbusInterface, const pldm::utils::PropertyValue& value)
{
    try
    {
        auto& bus = pldm::utils::DBusHandler::getBus();
        auto service = pldm::utils::DBusHandler().getService(
            objPath.c_str(), dbusInterface.c_str());
        auto method = bus.new_method_call(service.c_str(), objPath.c_str(),
                                          dbusInterface.c_str(),
                                          dbusMethod.c_str());
        auto val = std::get_if<std::vector<uint8_t>>(&value);
        method.append(*val);
        bus.call_noreply(method);
    }
    catch (const std::exception& e)
    {
        error("Failed to call the D-Bus Method ERROR={ERR_EXCEP}", "ERR_EXCEP",
              e);
        return;
    }
}

void pldm::responder::oem_ibm_platform::Handler::modifyPDROemActions(
    uint16_t entityType, uint16_t stateSetId)
{
    pldm::pdr::EntityType pdrEntityType = entityType;
    pldm::pdr::StateSetId pdrStateSetId = stateSetId;
    if ((pdrEntityType == (PLDM_ENTITY_CHASSIS_FRONT_PANEL_BOARD | 0x8000)) &&
        (pdrStateSetId == PLDM_OEM_IBM_PANEL_TRIGGER_STATE))
    {
        auto pdrs = pldm::utils::findStateEffecterPDR(0, pdrEntityType,
                                                      pdrStateSetId, pdrRepo);
        if (!std::empty(pdrs))
        {
            auto bitMap = responder::pdr_utils::fetchBitMap(pdrs);
            setBitmapMethodCall("/com/ibm/panel_app", "toggleFunctionState",
                                "com.ibm.panel", bitMap);
        }
    }
}

void pldm::responder::oem_ibm_platform::Handler::handleBootTypesAtPowerOn()
{
    BiosAttributeList biosAttrList;
    auto bootInitiator = getBiosAttrValue("pvm_boot_initiator_current");
    std::string restartCause;
    if (((bootInitiator != "HMC") || (bootInitiator != "Host")) &&
        !bootInitiator.empty())
    {
        try
        {
            restartCause =
                pldm::utils::DBusHandler().getDbusProperty<std::string>(
                    "/xyz/openbmc_project/state/host0", "RestartCause",
                    "xyz.openbmc_project.State.Host");
            setBootTypesBiosAttr(restartCause);
        }
        catch (const std::exception& e)
        {
            error(
                "Failed to set the D-bus property for the Host restart reason ERROR={ERR}",
                "ERR", e);
        }
    }
}

void pldm::responder::oem_ibm_platform::Handler::setBootTypesBiosAttr(
    const std::string& restartCause)
{
    BiosAttributeList biosAttrList;
    if (restartCause ==
        "xyz.openbmc_project.State.Host.RestartCause.ScheduledPowerOn")
    {
        biosAttrList.push_back(std::make_pair("pvm_boot_initiator", "Host"));
        setBiosAttr(biosAttrList);
        stateManagerMatch.reset();
    }
    else if (
        (restartCause ==
         "xyz.openbmc_project.State.Host.RestartCause.PowerPolicyAlwaysOn") ||
        (restartCause ==
         "xyz.openbmc_project.State.Host.RestartCause.PowerPolicyPreviousState"))
    {
        biosAttrList.push_back(std::make_pair("pvm_boot_initiator", "Auto"));
        setBiosAttr(biosAttrList);
        stateManagerMatch.reset();
    }
    else if (restartCause ==
             "xyz.openbmc_project.State.Host.RestartCause.HostCrash")
    {
        biosAttrList.push_back(std::make_pair("pvm_boot_initiator", "Auto"));
        biosAttrList.push_back(std::make_pair("pvm_boot_type", "ReIPL"));
        setBiosAttr(biosAttrList);
        stateManagerMatch.reset();
    }
}

void pldm::responder::oem_ibm_platform::Handler::handleBootTypesAtChassisOff()
{
    BiosAttributeList biosAttrList;
    auto bootInitiator = getBiosAttrValue("pvm_boot_initiator");
    auto bootType = getBiosAttrValue("pvm_boot_type");
    if (bootInitiator.empty() || bootType.empty())
    {
        error(
            "ERROR in fetching the pvm_boot_initiator and pvm_boot_type BIOS attribute values");
        return;
    }
    else if (bootInitiator != "Host")
    {
        biosAttrList.push_back(std::make_pair("pvm_boot_initiator", "User"));
        biosAttrList.push_back(std::make_pair("pvm_boot_type", "IPL"));
        setBiosAttr(biosAttrList);
    }
}

void pldm::responder::oem_ibm_platform::Handler::startStopTimer(bool value)
{
    if (value)
    {
        timer.restart(
            std::chrono::seconds(HEARTBEAT_TIMEOUT + HEARTBEAT_TIMEOUT_DELTA));
    }
    else
    {
        timer.setEnabled(value);
    }
}

void pldm::responder::oem_ibm_platform::Handler::setSurvTimer(uint8_t tid,
                                                              bool value)
{
    if ((hostOff == true) || (hostTransitioningToOff == true) ||
        (tid != HYPERVISOR_TID))
    {
        if (timer.isEnabled())
        {
            startStopTimer(false);
        }
        return;
    }
    if (value)
    {
        startStopTimer(true);
    }
    else if (!value && timer.isEnabled())
    {
        info(
            "setSurvTimer:LogginPel:hostOff={HOST_OFF} hostTransitioningToOff={HOST_TRANST_OFF} tid={TID}",
            "HOST_OFF", (bool)hostOff, "HOST_TRANST_OFF",
            (bool)hostTransitioningToOff, "TID", (uint16_t)tid);
        startStopTimer(false);
        pldm::utils::reportError(
            "xyz.openbmc_project.bmc.PLDM.setSurvTimer.RecvSurveillancePingFail");
    }
}

} // namespace oem_ibm_platform
} // namespace responder
} // namespace pldm
