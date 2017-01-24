#include <AMDTPwrProfTimerHelper.h>
#include <AMDTHelpers.h>
#include <AMDTCommonConfig.h>
#include <AMDTSharedBufferConfig.h>

extern uint8_t* g_pSharedBuffer;

// AllocateAndInitClientData
//
// Allocate and Initialise Client Data
int AllocateAndInitClientData(ClientData** ppClientData,
                              uint32 clientId,
                              cpumask_t affinity,
                              const ProfileConfig* config)
{
    ClientData* pClientData = NULL;

    // Allocate and construct ClientData
    pClientData = kmalloc(sizeof(ClientData), GFP_KERNEL);

    if (NULL == pClientData)
    {
        printk(KERN_WARNING "Power Profiler: Memory Allocation failure for timer \n");
        return -ENOMEM;
    }

    // initialise ClientData
    memset(pClientData, 0, sizeof(ClientData));

    pClientData->m_clientId                       = clientId;
    pClientData->m_osClientCfg.m_affinity         = affinity;
    pClientData->m_osClientCfg.m_paused           = false;
    pClientData->m_osClientCfg.m_stopped          = false;
    pClientData->m_configCount                    = config->m_samplingSpec.m_maskCnt;

    pClientData->m_header.m_pBuffer = kmalloc(HEADER_BUFFER_SIZE, false);

    if (NULL == pClientData->m_header.m_pBuffer)
    {
        printk(KERN_WARNING "Power Profiler: Memory Allocation failure for Core Buffer\n");
        return -ENOMEM;
    }

    pClientData->m_header.m_recCnt = 0;
    atomic_set(&pClientData->m_header.m_currentOffset, 0);
    atomic_set(&pClientData->m_header.m_consumedOffset, 0);

    *ppClientData = pClientData;
    return 0;
}

// IntializeCoreData
//
// Initialise and  per core data
int IntializeCoreData(CoreData* pCoreClientData,
                      int index,
                      bool* isFirstConfig,
                      uint32 clientId,
                      uint32 coreId,
                      ProfileConfig* config)
{
    uint64 phyCoreMask = 0;
    uint64 coreSpecficMask = 0;
    uint32 masterCoreBufferSize = sizeof(PageBuffer) + PWRPROF_MASTER_CORE_BUFFER_SIZE;


    uint32 offset = PWRPROF_SHARED_METADATA_SIZE; // initial 4096 bytes for future use

    if (NULL == g_pSharedBuffer)
    {
        printk("Power Profiler: shared memory is not yet allocated.\n");
        return -ENOMEM;
    }

    // currently blindly clearing the g_coreClientData
    memset(pCoreClientData, 0, sizeof(CoreData));

    pCoreClientData->m_clientId             = clientId;
    pCoreClientData->m_sampleId             = index + 1;
    pCoreClientData->m_samplingInterval     = config->m_samplingSpec.m_samplingPeriod;
    pCoreClientData->m_recLen               = 0;
    pCoreClientData->m_coreId               = coreId;

    if(0 == index)
    {
        pCoreClientData->m_bufferSize = PWRPROF_MASTER_CORE_BUFFER_SIZE;
    }
    else
    {
        pCoreClientData->m_bufferSize = PWRPROF_NONMASTER_CORE_BUFFER_SIZE;
        offset += masterCoreBufferSize + ((index-1) * (sizeof(PageBuffer) + pCoreClientData->m_bufferSize));
    }

    pCoreClientData->m_pCoreBuffer = (PageBuffer*)(g_pSharedBuffer + offset);

    if (NULL == pCoreClientData->m_pCoreBuffer)
    {
        printk("Power Profiler: Failed to get shared memory for pCoreBuffer\n");
        return -ENOMEM;
    }

    // initialize the meta data shared between driver and backend
    pCoreClientData->m_pCoreBuffer->m_recCnt = 0;
    atomic_set(&pCoreClientData->m_pCoreBuffer->m_currentOffset, 0);
    atomic_set(&pCoreClientData->m_pCoreBuffer->m_consumedOffset, 0);
    atomic_set(&pCoreClientData->m_pCoreBuffer->m_maxValidOffset, 0);
    pCoreClientData->m_pCoreBuffer->m_fill = 0xdeadbeef;

    // m_pCoreBuffer::m_pBuffer should point to the start of the data buffer
    pCoreClientData->m_pCoreBuffer->m_pBuffer = g_pSharedBuffer + offset + sizeof(PageBuffer);

    // TODO: Is it OK to do memset for many pages?
    memset(pCoreClientData->m_pCoreBuffer->m_pBuffer, 0, pCoreClientData->m_bufferSize);

    pCoreClientData->m_pOsData = kmalloc(sizeof(OsCoreCfgData), GFP_KERNEL);

    if (NULL == pCoreClientData->m_pOsData)
    {
        printk(KERN_WARNING "Power Profiler: Failed allocating memory for timer\n");
        return -ENOMEM;
    }

    memset(pCoreClientData->m_pOsData, 0, sizeof(OsCoreCfgData));

    // Callback timer
    pCoreClientData->m_pOsData->m_interval = ktime_set(0, config->m_samplingSpec.m_samplingPeriod * 1000000);
    pCoreClientData->m_samplingInterval = config->m_samplingSpec.m_samplingPeriod;
    pCoreClientData->m_profileType = config->m_samplingSpec.m_profileType;


    // smu configure
    pCoreClientData->m_smuCfg = NULL;

    // Check for core energy counters which are physical core specific
    if(config->m_apuCounterMask & (1ULL << COUNTERID_CORE_ENERGY))
    {
        if(!HelpPwrIsSmtEnabled() || (0 == (coreId % 2)))
        {
            phyCoreMask = (1ULL << COUNTERID_CORE_ENERGY);
        }
    }

    if (true == *isFirstConfig)
    {
        // Configuration for master core
        pCoreClientData->m_counterMask = config->m_apuCounterMask;

        pCoreClientData->m_smuCfg = kmalloc(sizeof(SmuList), GFP_KERNEL);

        if (NULL == pCoreClientData->m_smuCfg)
        {
            printk(KERN_WARNING "Power Profiler: Memory Allocation failure for SmuList \n");
            return -ENOMEM;
        }

        memset(pCoreClientData->m_smuCfg, 0, sizeof(SmuList));

        if ((0 < config->m_activeList.m_count) &&
            (false == FillSmuAccessData(&config->m_activeList, pCoreClientData->m_smuCfg)))
        {
            printk(KERN_WARNING "Power Profiler: Error while configuring SMU \n");
        }
        else
        {
            // Configure all the required smu
            ConfigureSmu(pCoreClientData->m_smuCfg, true);
        }

        *isFirstConfig = false;
    }
    else
    {
        coreSpecficMask = config->m_apuCounterMask & PWR_PERCORE_COUNTER_MASK;
        pCoreClientData->m_counterMask = coreSpecficMask | phyCoreMask;
    }

    return 0;
}

// FreeClientData
//
// Free Memmory set by client data
void FreeClientData(ClientData* pClientData)
{
    if (NULL != pClientData)
    {
        if (NULL != pClientData->m_header.m_pBuffer)
        {
            kfree(pClientData->m_header.m_pBuffer);
            pClientData->m_header.m_pBuffer = NULL;
        }

        kfree(pClientData);
    }

    return;
}

// FreeCoreData
//
// Free memory allocated to CoreData
//
void FreeCoreData(CoreData* pCoreClientData)
{
    if (NULL != pCoreClientData)
    {
        if (NULL != pCoreClientData->m_pCoreBuffer)
        {
            pCoreClientData->m_pCoreBuffer = NULL;
        }

        if (NULL != pCoreClientData->m_smuCfg)
        {
            kfree(pCoreClientData->m_smuCfg);
            pCoreClientData->m_smuCfg = NULL;
        }

        if (NULL != pCoreClientData->m_pOsData)
        {
            kfree(pCoreClientData->m_pOsData);
            pCoreClientData->m_pOsData = NULL;
        }
    }

    return;
}
