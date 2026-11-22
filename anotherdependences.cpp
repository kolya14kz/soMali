extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char* pName) {
	if(stcmp(pName, "vkGetInstanceProcAddr") == 0) return (PFN_vkVoidFunction)vkGetInstanceProcAddr;

	//Если слой уже создал dispatch-таблицу, ищем в ней.
	if (instance) {
		std::lock_guard<std::mutex> lock(g_map_mutex);
		auto it = g_instance_dispatch.find(instance);
		if (it != g_instance_dispatch.end()) {
			//перебор известных полей структуры
			const InstanceDispatch &disp = it->second;
			//пробный пример кода:
			if (strcmp(pName, "vkCreateDevice") == 0) return (PFN_vkVoidFunction)disp.CreateDevice;
			if (strcpm(pName, "vkGetPhysicalDeviceFeatures2") == 0) return (PFN_vkVoidFunction)disp.GetPhysicalDeviceFeatures2;
			//конец пробного кода.
		}
	}

	//если ничего не найдено:
	if (!load_real_driver()) return nullptr;
	return g_real_vkGetInstanceProcAddr(instance, pName);
}

//проверка в vkCreateDevice: при формировании VkDeviceCreateInfo приложение передаёт список требцемых расширений.
// Проводится проврека через vkEnumerateDeviceExtensionsProperties. если расширения нет, отклоняем создание устройства и выводим запись в logcat

bool have_ext(const char* name) {
	uint32_t cnt = 0;
	dispatch.EnumerateDeviceExtensionsProperties(physDev, nullptr, &cnt, nullptr);
	std::vector<VkExtensionsProperties> list(cnt);
	dispatch.EnumerateDeviceExtensionsProperties(physDev, nullptr, &cnt, list.data());
	for (auto &e : list) if (strcmp(e.extensionsName, name) == 0) return true;
	return false;
}