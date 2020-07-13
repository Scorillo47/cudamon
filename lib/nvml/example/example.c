#include <stdio.h>
#include <Windows.h>
#include <nvml.h>

const BOOL bTraceEnabled = FALSE;

#define MSG(...) { if (bTraceEnabled) { printf(__VA_ARGS__); } }

typedef struct GpuProcessStatisticsT
{
	DWORD processID;
	unsigned int powerUsage;
	unsigned long long memoryUsageInBytes;
} GpuProcessStatistics;


const char * convertToComputeModeString(nvmlComputeMode_t mode)
{
    switch (mode)
    {
        case NVML_COMPUTEMODE_DEFAULT:
            return "Default";
        case NVML_COMPUTEMODE_EXCLUSIVE_THREAD:
            return "Exclusive_Thread";
        case NVML_COMPUTEMODE_PROHIBITED:
            return "Prohibited";
        case NVML_COMPUTEMODE_EXCLUSIVE_PROCESS:
            return "Exclusive Process";
        default:
            return "Unknown";
    }
}

BOOL QueryDeviceHardware(unsigned int deviceId, nvmlDevice_t device)
{
	nvmlReturn_t result;
	nvmlPciInfo_t pci;
	nvmlComputeMode_t compute_mode;
	char name[NVML_DEVICE_NAME_BUFFER_SIZE];

	result = nvmlDeviceGetName(device, name, NVML_DEVICE_NAME_BUFFER_SIZE);
	if (NVML_SUCCESS != result)
	{
		MSG("ERROR: Failed to get name of device %i: %s\n", deviceId, nvmlErrorString(result));
		return FALSE;
	}

	// pci.busId is very useful to know which device physically you're talking to
	// Using PCI identifier you can also match nvmlDevice handle to CUDA device.
	result = nvmlDeviceGetPciInfo(device, &pci);
	if (NVML_SUCCESS != result)
	{
		MSG("ERROR: Failed to get pci info for device %i: %s\n", deviceId, nvmlErrorString(result));
		return FALSE;
	}

	MSG("- Name for device with ID %d = %s [%s]\n", deviceId, name, pci.busId);

	// This is a simple example on how you can modify GPU's state
	result = nvmlDeviceGetComputeMode(device, &compute_mode);
	if (NVML_ERROR_NOT_SUPPORTED == result)
	{
		MSG("ERROR: This is not CUDA capable device\n");
	}
	else if (NVML_SUCCESS != result)
	{
		MSG("ERROR: Failed to get compute mode for device %i: %s\n", deviceId, nvmlErrorString(result));
		return FALSE;
	}
	else
	{
		MSG("- Device compute mode = %s\n", convertToComputeModeString(compute_mode));
	}

	return TRUE;
}

BOOL QueryDeviceUsermodeProcessStatistics(unsigned int deviceId, nvmlDevice_t device, GpuProcessStatistics * pProcStatsArray)
{
	nvmlReturn_t result;
	unsigned int infoCount = 0;
	nvmlProcessInfo_t* infos = NULL;
	unsigned int power = 0;

	result = nvmlDeviceGetPowerUsage(device, &power);
	if (NVML_SUCCESS != result)
	{
		MSG("ERROR: Failed to query nvmlDeviceGetPowerUsage: %s\n", nvmlErrorString(result));
		goto Error;
	}

	result = nvmlDeviceGetComputeRunningProcesses(device, &infoCount, NULL);
	if (NVML_SUCCESS == result)
	{
		// No process running 
		MSG("- No processes running\n");
		return TRUE;
	}

	if (NVML_ERROR_INSUFFICIENT_SIZE != result)
	{
		MSG("ERROR: Failed to query nvmlDeviceGetComputeRunningProcesses: %s\n", nvmlErrorString(result));
		goto Error;
	}

	infos = (nvmlProcessInfo_t*)calloc(infoCount, sizeof(nvmlProcessInfo_t));
	result = nvmlDeviceGetComputeRunningProcesses(device, &infoCount, infos);
	if ((NVML_SUCCESS != result) && (NVML_ERROR_INSUFFICIENT_SIZE != result))
	{
		MSG("ERROR: Failed to query nvmlDeviceGetComputeRunningProcesses: %s\n", nvmlErrorString(result));
		goto Error;
	}

	unsigned int pid = 0;
	unsigned long long totalGpuMemUsage = 0;
	for (int i = 0; i < (int)infoCount; i++)
	{
		MSG("- processInfo[%d]: pid = %u, usedGpuMemory = %I64u\n", i, infos[i].pid, infos[i].usedGpuMemory);
		totalGpuMemUsage += infos[i].usedGpuMemory;

		// Get the last PID and assume that it consumes all the memory on the GPU
		// We need to do this since GPUMCM current counter code does not support accounting for more than one process per GPU 
		pid = infos[i].pid;
	}

	pProcStatsArray->processID = pid;
	pProcStatsArray->memoryUsageInBytes = totalGpuMemUsage;
	pProcStatsArray->powerUsage = power;

	free(infos);
	return TRUE;

Error:

	free(infos);
	return FALSE;
}


int main()
{
    nvmlReturn_t result;
    unsigned int device_count, deviceId;
	GpuProcessStatistics * pProcStatsArray = NULL;

    // First initialize NVML library
    result = nvmlInit();
    if (NVML_SUCCESS != result)
    { 
        MSG("ERROR: Failed to initialize NVML: %s\n", nvmlErrorString(result));
        return 1;
    }

    result = nvmlDeviceGetCount(&device_count);
    if (NVML_SUCCESS != result)
    { 
        MSG("ERROR: Failed to query device count: %s\n", nvmlErrorString(result));
        goto Error;
    }
    MSG("Found %d device%s\n\n", device_count, device_count != 1 ? "s" : "");

	pProcStatsArray = (GpuProcessStatistics *)calloc(device_count, sizeof(GpuProcessStatistics));

    MSG("Listing devices:\n");    
    for (deviceId = 0; deviceId < device_count; deviceId++)
    {
        nvmlDevice_t device;
		BOOL res;

		MSG("- GPU index = %d\n", deviceId);

        // Query for device handle to perform operations on a device
        // You can also query device handle by other features like:
        // nvmlDeviceGetHandleBySerial
        // nvmlDeviceGetHandleByPciBusId
        result = nvmlDeviceGetHandleByIndex(deviceId, &device);
        if (NVML_SUCCESS != result)
        { 
            MSG("ERROR: Failed to get handle for device %i: %s\n", deviceId, nvmlErrorString(result));
            goto Error;
        }

		res = QueryDeviceHardware(deviceId, device);
		if (!res)
		{
			MSG("ERROR: Failed to query hardware for device %i: %s\n", deviceId, nvmlErrorString(result));
			goto Error;
		}

		res = QueryDeviceUsermodeProcessStatistics(deviceId, device, &(pProcStatsArray[deviceId]));
		if (!res)
		{
			MSG("ERROR: Failed to query user-mode process GPU stats for device %i: %s\n", deviceId, nvmlErrorString(result));
			goto Error;
		}
    }

	for (deviceId = 0; deviceId < device_count; deviceId++)
	{
		printf("%sGpuIndex=%u,ProcessId=%u,PowerUsage=%u,MemoryUsageInBytes=%I64u",
			(deviceId > 0)? ";":"", 
			deviceId,
			pProcStatsArray[deviceId].processID,
			pProcStatsArray[deviceId].powerUsage,
			pProcStatsArray[deviceId].memoryUsageInBytes
		);
	}

	printf("\n");

    result = nvmlShutdown();
    if (NVML_SUCCESS != result)
        MSG("Failed to shutdown NVML: %s\n", nvmlErrorString(result));

    MSG("All done.\n");
	free(pProcStatsArray);
	return 0;

Error:
    result = nvmlShutdown();
    if (NVML_SUCCESS != result)
        MSG("Failed to shutdown NVML: %s\n", nvmlErrorString(result));
	free(pProcStatsArray);
	return 1;
}
