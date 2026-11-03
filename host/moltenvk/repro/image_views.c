/* STEAMAC-62/64/3: never pass empty slice ranges or incompatible formats to
 * Metal. Deliberately invalid guest views run without Vulkan validation; each
 * gets a separate process so a pre-fix Metal abort identifies its input. Valid
 * remaining-layer and 3D-slice controls must still create successfully. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#define CK(x) do { VkResult r = (x); if (r) { fprintf(stderr, "FAIL %s = %d\n", #x, r); exit(1); } } while (0)

static int run_case(const char *name)
{
	const int volume = strstr(name, "3d") != NULL;
	const int invalid = strncmp(name, "invalid-", 8) == 0;
	VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3 };
	VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
	VkInstance instance;
	CK(vkCreateInstance(&ici, NULL, &instance));
	uint32_t n = 1;
	VkPhysicalDevice pd;
	CK(vkEnumeratePhysicalDevices(instance, &n, &pd));
	float priority = 1;
	VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &priority };
	VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
	VkDevice dev;
	CK(vkCreateDevice(pd, &dci, NULL, &dev));
	VkImageCreateInfo ci = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.flags = volume ? VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT : VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,
		.imageType = volume ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D,
		.format = VK_FORMAT_R8G8B8A8_UNORM, .extent = { 16, 16, volume ? 4 : 1 },
		.mipLevels = 3, .arrayLayers = volume ? 1 : 4, .samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT };
	VkImage image;
	CK(vkCreateImage(dev, &ci, NULL, &image));
	VkMemoryRequirements mr;
	vkGetImageMemoryRequirements(dev, image, &mr);
	uint32_t type = 0;
	while (!(mr.memoryTypeBits & (1u << type))) type++;
	VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = type };
	VkDeviceMemory memory;
	CK(vkAllocateMemory(dev, &ai, NULL, &memory));
	CK(vkBindImageMemory(dev, image, memory, 0));
	VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = image,
		.viewType = VK_IMAGE_VIEW_TYPE_2D, .format = ci.format,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, volume ? 1 : 3, VK_REMAINING_ARRAY_LAYERS } };
	if (strstr(name, "zero")) vi.subresourceRange.layerCount = 0;
	if (strstr(name, "end")) vi.subresourceRange.baseArrayLayer = volume ? 2 : 4;
	if (strstr(name, "past")) vi.subresourceRange.baseArrayLayer = volume ? 3 : 5;
	if (strstr(name, "wide")) vi.subresourceRange.layerCount = 65536;
	if (strstr(name, "format")) vi.format = VK_FORMAT_R8_UNORM;
	if (strstr(name, "array")) {
		vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
		vi.subresourceRange.baseArrayLayer = 0;
	}
	if (strstr(name, "volume")) {
		vi.viewType = VK_IMAGE_VIEW_TYPE_3D;
		vi.subresourceRange.baseArrayLayer = 0;
	}
	fprintf(stderr, "image_views: %s\n", name);
	VkImageView view = VK_NULL_HANDLE;
	VkResult result = vkCreateImageView(dev, &vi, NULL, &view);
	if ((!invalid && result != VK_SUCCESS) || (invalid && result != VK_ERROR_FORMAT_NOT_SUPPORTED)) {
		fprintf(stderr, "FAIL %s: result %d, expected %d\n", name, result,
			invalid ? VK_ERROR_FORMAT_NOT_SUPPORTED : VK_SUCCESS);
		return 1;
	}
	if (result == VK_SUCCESS) vkDestroyImageView(dev, view, NULL);
	vkDestroyImage(dev, image, NULL);
	vkFreeMemory(dev, memory, NULL);
	vkDestroyDevice(dev, NULL);
	vkDestroyInstance(instance, NULL);
	printf("OK image_views %s\n", name);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc == 2) return run_case(argv[1]);
	const char *cases[] = { "remaining-2d", "remaining-2d-array", "remaining-3d", "remaining-3d-array", "remaining-3d-volume",
		"invalid-zero-2d", "invalid-end-2d", "invalid-past-2d", "invalid-wide-2d",
		"invalid-zero-3d", "invalid-end-3d", "invalid-past-3d", "invalid-format-2d" };
	int failed = 0;
	for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		pid_t child = fork();
		if (child == -1) { perror("fork"); return 1; }
		if (!child) { execl(argv[0], argv[0], cases[i], (char *)NULL); perror("execl"); _exit(1); }
		int status;
		if (waitpid(child, &status, 0) == -1) { perror("waitpid"); return 1; }
		if (!WIFEXITED(status) || WEXITSTATUS(status)) {
			fprintf(stderr, "FAIL image_views %s: child status 0x%x\n", cases[i], status);
			failed = 1;
		}
	}
	return failed;
}
