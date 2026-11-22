#define VK_USE_PLATFORM_ANDRIOD_KHR
#include <vulkan/vulkan.h>
#include <android/log.h>
#include <dlfcn.h>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <mutex>
#include <vector>
#include <thread>
#include <future>
#include <atomic>
#include <fstream>
#include <filesystem>

#define LOG_TAG "VulkanMaliWrapper"
#define LOGI(...)
__android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...)
__android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// loader-side драйвер хэндла
static void* real_driver = nullptr;

static void load_real_driver()
{
	if (real_driver) return;
	// уже загружено
	const char* drv_path ="/vendor/lib64/hw/vulkan.mtk.so";
	real_driver = dlopen(drv_path, RTLD_NOW | RTLD_LOCAL);
	if (!real_driver) {
		LOGE("Failed to dlopen %s: %s", drv_path, dlerror());
		std::abort();
	}
}

// helper для получения реальных выходных точек
static PFN_vkVoidFunction get_real_proc(const char* name)
{
	load_real_driver();
	PFN_vkVoidFunction fp = (PFN_vkVoidFunction) dlsym(real_driver, name);
	if (!fp) LOGE("Real driver missing %s", name);
	return fp;
}

// Dispatch-table контейнеры
struct InstanceDispatch {
	PFN_vkCreateInstance CreateInstance = nullptr;
	PFN_vkDestroyInstance DestroyInstance = nullptr;
	PFN_vkCreateDevice CreateDevice = nullptr;
	PFN_vkEnumerateInstanceVersion EnumerateInstanceVersion = nullptr;
	PFN_vkEnumerateInstanceLayerProperties EnumerateInstanceLayerProperties = nullptr;
	// добавить другиие instance уровневые функции
};

struct DeviceDispatch {
	PFN_vkAllocateMemory AllocateMemory = nullptr;
	PFN_vkFreeMemory FreeMemory = nullptr;
	PFN_vkCreateGraphicsPipelines CreateGraphicsPipelines = nullptr;
	PFN_vkQueueSubmit QueueSubmit = nullptr;
	PFN_vkCreatePipelineCache CreatePipelineCache = nullptr;
	PFN_vkDestroyPipelineCache DestroyPipelineCache = nullptr;
	PFN_vkGetPipelineCacheData GetPipelineCacheData = nullptr;
	PFN_vkCreateShaderModule CreateShaderModule = nullptr;
	PFN_vkDestroyShaderModule DestroyShaderModule = nullptr;
	PFN_vkGetDeviceProcAddr GetDeviceProcAddr = nullptr;
	//добавить остальные параметры
};

static std::unordered_map<VkInstance, InstanceDispatch> g_instanceDispatch;
static std::unordered_map<VkDevice, DeviceDispatch> g_deviceDispatch;
static std::mutex g_dispatchMutex;

// VMA интеграция
#define VMA_IMPLEMENTATION
#define VMA_STATIC_VULKAN_FUNCTIONS
#include "vk_mem_alloc.h"

static VmaAllocator g_vmaAllocator = VK_NULL_HANDLE;

// инициализиреут VMA по одному ч-з VkInstance (первый созданный instance)
static void init_vma(VkInstance instance, VkPhysicalDevice physDev, VkDevice deivce)
{
	VmaAllocatorCreateInfo allocInfo{};
	allocInfo.physicalDevice = physDev;
	allocInfo.device = device;
	allocInfo.instance = instance;
	allocInfo.flags = VMA_ALLOCATOR_CREATE_EXTERNALLY_SYNCHRONIZED_BIT;
	VkResult res = vmaCreateAllocator(&allocInfo, &g_vmaAllocator);
	if (res != VK_SUCCESS) {
		LOGE("VMA initialization failed: %d", res);
		std::abort();
	}
}

//Helper возвращающий allocation запросы к VMA
static VkResult vma_allocate_memory(VkDevice device,
const VkMemoryAllocateInfo* pAllocateInfo,
const VkAllocationCallbacks* pAllocator, VkdeviceMemory* pMemory)
{
	VmaAllocationCreatleInfo createInfo{};
	createInfo.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
	VmaAllocation allocation;
	VkResult res = vmaAllocationMemory(g_vmaAllocator, pAllocateInfo, &createInfo, %allocation, nullptr);
	if (res == VK_SUCCESS) {
		*pMemory = (VkDeviceMemory) allocation;
	}
	return res;
}

static void vma_free_memory(VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks* pAllocator)
{
	VmaAllocation allocation = (VmaAllocation) memory;
	vmaFreeMemory(g_vmaAllocator, allocation);
}

// асинхронный pipeline cache / shader-module disk cache
static const std::string CACHE_DIR = std::string(std::getenv("HOME")) + "/.cache/vulkan_mali_wrapper";

struct AsyncCacheTask {
	std::future<void> fut;
	std::atomic<bool> done{false};
};

static std::unordered_map<VkPipelineCache, AsyncCacheTask> g_cacheTasks;
static std::mutex g_cacheMutex;

//записывает данные кэша в память фоном
static void async_write_cache(VkPipelineCache cache, const void* data, size_t size)
{
	std::lock_guard<std::mutex> lk(g_cacheMutex);
	AsyncCacheTask task;
	task.fut = std::async(std::launch::async, [=]{
		std::filesystem::create_directories(CACHE_DIR);
		std::string path = CACHE_DIR + "/pcache_" + std::to_string(reinterpret_cast<uintptr_t>(cache)) + ".bin";
		std::ofstream ofs(path, std::ios::binary);
		ofs.write(reinterpret_cast<const char*>(data), size);
		ofs.close();
		task.done = true;
	});
	g_cacheTasks.emplace(cache, std::move(task));
}

static bool async_load_cache(VkPipelineCache cache, std::vector<char>& outData)
{
	std::string path = CACHE_DIR + "/pcache_" + std::to_string(reinterpret_cast<uintptr_t>(cache)) + ".bin";
	if (!std::filesystem::exists(path)) return false;
	std::ifstream ifs(path, std::ios::binary | std::ios::ate);
	std::streamsize sz = ifs.tellg();
	ifs.seekg(0, std::ios::beg);
	outData.resize(static_cast<size_t>(sz));
	if (!ifs.read(outData.data(), sz)) return false;
	return true;
}

// перехваченные Vulkan entry points
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(
	const VkInstanceCreateInfo* pCreateInfo,
	const VkAllocationCallbacks* pAllocator,
	VkInstance* pInstance)
{
	LOGI("vkCreateInstance called");
	//возвращает настоящему драйверу
	auto fp = (PFN_vkCreateInstance) get_real_proc("vkCreateInstance");
	VkResult res = fp(pCreateInfo, pAllocator, pInstance);
	if (res != VK_SUCCESS) return res;

	InstanceDispatch dispatch{};
	dispatch.CreateInstance = fp;
	dispatch.DestroyInstance = (PFN_vkDestroyInstance) get_real_proc("vkDestroyInstance");
	dispatch.CreateDevice = (PFN_vkCreateDevice) get_real_proc("vkCreateDevice");
	dispatch.EnumerateInstanceVersion = (PFN_vkEnumerateInstanceVersion) get_real_proc("vkEnumerateInstanceVersion");
	dispatch.EnumerateInstanceLayerProperties = (PFN_vkEnumeratesInstanceLayerProperties) get_real_proc("vkEnumeratesInstanceLayerProperties");
	dispatch.GetInstanceProcAddr = (PFN_vkGetInstanceProcAddr) get_real_proc("vkGetInstanceProcAddr");

	std::lock_guard<std::mutex> lk(g_dispatchMutex);
	g_instanceDispatch.emplace(*pInstance, std::move(dispatch));
	return VK_SUCCESS;
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkDestroyInstance(
	VkInstance instance,
	const VkAllocationCallbacks* pAllocator)
{
	LOGI("vkDestroyInstance");
	std::lock_guard<std::mutex> lk(g_dispatchMutex);
	g_instanceDispatch.erase(instance);
	auto fp = (PFN_vkDestroyInstance) get_real_proc("vkdestroyInstance");
	return fp(instance, pAllocator);
}

// vkCreateDevice - инициализирует VMA и async-cache подмогу
extern "C" VKAPI_ATTR Vkresult VKAPI_CALL vkCreateDevice(
	VkPhysicalDeive physicalDevice,
	const VkDeviceCreateInfo* pCreateInfo,
	const VkAllocationCallbacks* pAllocator,
	VkDevice* pDevice)
{
	LOGI("vkCreateDevice intercepted");
	// резолвит настоящие функции через instance (physicalDevice принадлежит instance который уже хранится)
	VkInstance dummyInst = VK_NULL_HANDLE; //нужна привязка к реальному драйверу; безопаснее брать через загрузчик
	auto fp = (PFN_vkCreateDevice) get_real_proc("vkCreatedevice");
	VkResult res = fp(physicalDevice, pCreateInfo, pAllocator, pDevice);
	if (res != VK_SUCCESS) return res;

	//пробная таблица
DeviceDispatch ddispatch{}; 
ddispatch.AllocateMemory = (PFN_vkAllocateMemory) get_real_proc("vkAllocateMemory");
ddispatch.FreeMemory = (PFN_vkFreeMemory) get_real_proc("vkFreeMemory");
ddispatch.CreateGraphicsPipelines = (PFN_vkCreateCraphicsPipelines) get_real_proc("vkCreateGraphicsPipelines");
ddispatch.QueueSubmit = (PFN_vkQueueSubmit) get_real_proc("vkQueueSubmit");
ddispatch.CreatePipelineCache = (PFN_vkCreatePipelineCache) get_real_proc("vkCreatePipelineCache");
ddispatch.DestroyPipelineCache = (PFN_vkDestroyPipelineCache) get_real_proc("vkDestroyPipelineCache");
ddispatch.GetPipelineCacheData = (PFN_vkCreatePipelineCacheData) get_real_proc("vkGetPipelinecacheData");
ddispatch.CreateShaderModule = (PFN_vkCreateShaderModule) get_real_proc("vkCreateShaderModule");
ddispatch.DestroyShaderModule = (PFN_vkDestroyShaderModule) get_real_proc("vkDestroyShaderModule");
ddispatch.GetDeviceProcAddr = (PFN_vkGetDeviceProcAddr) get_real_proc("vkGetDeviceProcAddr").

{
	std::lock_guard<std::mutex> lk(g_dispatchMutex);
	g_deviceDispatch.emplace(*pDevice, std::move(ddispatch));
}

//инициализация VMA
static std::once_flag vma_once;
std::call_once(vma_once, [&]{
	//нужно VkInstance для VMA; найти через physicalDevice -> instance mapping. загрузчик гарантирует что тот же VkPhysicalDevice точно придёт из одного VkInstance
	VkInstance instance = VK_NULL_HANDLE;
	{
		//создаётся временная структура запроса
		VkPhysicalDeviceProperties props{};
		vkGetPhysicalDeviceProperties(physicalDevice, &props);
	}
	init_vma(instance, physicalDevice, *pDevice);
	LOGI("VMA allocator created");
});

return VK_SUCCESS;
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkAllocateMemory(
	VkDevice device,
	const VkMemoryAllocateInfo* pAllocateInfo,
	const VkAllocationCallbacks* pAllocator,
	VkDeviceMemory* pMemory)
{
	LOGI("vkAllocateMemory -> VMA");
	return vma_allocate_memory(deivce, pAllocateInfo, pAllocator, pMemory);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkFreeMemory(
	VkDevice device,
	VkDeviceMemory memory,
	const VkAllocationCallbacks* pAllocator)
{
	LOGI("vkFreeMemory -> VMA");
	vma_free_memory(device, memory, pAllocator);
}

// vkCreateGraphicsPipelines - логгированиие и pass-through опционально
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkCreateGraphicsPipelines(
	VkDevice device,
	uint32_t createInfoCount,
	const VkGraphicsPipelineCreateInfo* pCreateInfos,
	const VkAllocationCallbacks* pAllocator,
	VkPipeline* pPipelines)
{
	LOGI("vkCreateGraphicsPipelines count=%u", createInfoCount);
	std::lock_guard<std::mutex> lk(g_dispatchMutex);
	auto it = g_deviceDispatch.find(device);
	if (it == g_deviceDispatch.end()) {
		LOGE("Device dispatch missing in vkCreateGraphicsPipelines");
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	return it->second.CreateGraphicsPipelines(device, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelibes);
}

// vkQueueSubmit

//пока конец