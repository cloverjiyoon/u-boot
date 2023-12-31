// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2020 Stefan Roese <sr@denx.de>
 */

#include <common.h>
#include <image.h>
#include <log.h>
#include <malloc.h>
#include <asm/sections.h>
#include <spl.h>

#include <lzma/LzmaTypes.h>
#include <lzma/LzmaDec.h>
#include <lzma/LzmaTools.h>

#define LZMA_LEN	(1 << 20)

static void spl_parse_legacy_validate(uintptr_t start, uintptr_t size)
{
	uintptr_t spl_start = (uintptr_t)_start;
	uintptr_t spl_end = (uintptr_t)_image_binary_end;
	uintptr_t end = start + size;

	if ((start >= spl_start && start < spl_end) ||
	    (end > spl_start && end <= spl_end) ||
	    (start < spl_start && end >= spl_end) ||
	    (start > end && end > spl_start))
		panic("SPL: Image overlaps SPL\n");

	if (size > CONFIG_SYS_BOOTM_LEN)
		panic("SPL: Image too large\n");
}

int spl_parse_legacy_header(struct spl_image_info *spl_image,
			    const struct legacy_img_hdr *header)
{
	u32 header_size = sizeof(struct legacy_img_hdr);

	/* check uImage header CRC */
	if (IS_ENABLED(CONFIG_SPL_LEGACY_IMAGE_CRC_CHECK) &&
	    !image_check_hcrc(header)) {
		puts("SPL: Image header CRC check failed!\n");
		return -EINVAL;
	}

	if (spl_image->flags & SPL_COPY_PAYLOAD_ONLY) {
		/*
		 * On some system (e.g. powerpc), the load-address and
		 * entry-point is located at address 0. We can't load
		 * to 0-0x40. So skip header in this case.
		 */
		spl_image->load_addr = image_get_load(header);
		spl_image->entry_point = image_get_ep(header);
		spl_image->size = image_get_data_size(header);
	} else {
		spl_image->entry_point = image_get_ep(header);
		/* Load including the header */
		spl_image->load_addr = image_get_load(header) -
			header_size;
		spl_image->size = image_get_data_size(header) +
			header_size;
	}

#ifdef CONFIG_SPL_LEGACY_IMAGE_CRC_CHECK
	/* store uImage data length and CRC to check later */
	spl_image->dcrc_data = image_get_load(header);
	spl_image->dcrc_length = image_get_data_size(header);
	spl_image->dcrc = image_get_dcrc(header);
#endif

	spl_image->os = image_get_os(header);
	spl_image->name = image_get_name(header);
	debug(SPL_TPL_PROMPT
	      "payload image: %32s load addr: 0x%lx size: %d\n",
	      spl_image->name, spl_image->load_addr, spl_image->size);

	spl_parse_legacy_validate(spl_image->load_addr, spl_image->size);
	spl_parse_legacy_validate(spl_image->entry_point, 0);

	return 0;
}

/*
 * This function is added explicitly to avoid code size increase, when
 * no compression method is enabled. The compiler will optimize the
 * following switch/case statement in spl_load_legacy_img() away due to
 * Dead Code Elimination.
 */
static inline int spl_image_get_comp(const struct legacy_img_hdr *hdr)
{
	if (IS_ENABLED(CONFIG_SPL_LZMA))
		return image_get_comp(hdr);

	return IH_COMP_NONE;
}

int spl_load_legacy_img(struct spl_image_info *spl_image,
			struct spl_boot_device *bootdev,
			struct spl_load_info *load, ulong offset,
			struct legacy_img_hdr *hdr)
{
	__maybe_unused SizeT lzma_len;
	__maybe_unused void *src;
	ulong dataptr;
	int ret;

	/*
	 * If the payload is compressed, the decompressed data should be
	 * directly write to its load address.
	 */
	if (spl_image_get_comp(hdr) != IH_COMP_NONE)
		spl_image->flags |= SPL_COPY_PAYLOAD_ONLY;

	ret = spl_parse_image_header(spl_image, bootdev, hdr);
	if (ret)
		return ret;

	/* Read image */
	switch (spl_image_get_comp(hdr)) {
	case IH_COMP_NONE:
		dataptr = offset;

		/*
		 * Image header will be skipped only if SPL_COPY_PAYLOAD_ONLY
		 * is set
		 */
		if (spl_image->flags & SPL_COPY_PAYLOAD_ONLY)
			dataptr += sizeof(*hdr);

		load->read(load, dataptr, spl_image->size,
			   (void *)(unsigned long)spl_image->load_addr);
		break;

	case IH_COMP_LZMA:
		lzma_len = LZMA_LEN;

		/* dataptr points to compressed payload  */
		dataptr = offset + sizeof(*hdr);

		debug("LZMA: Decompressing %08lx to %08lx\n",
		      dataptr, spl_image->load_addr);
		src = malloc(spl_image->size);
		if (!src) {
			printf("Unable to allocate %d bytes for LZMA\n",
			       spl_image->size);
			return -ENOMEM;
		}

		load->read(load, dataptr, spl_image->size, src);
		ret = lzmaBuffToBuffDecompress((void *)spl_image->load_addr,
					       &lzma_len, src, spl_image->size);
		if (ret) {
			printf("LZMA decompression error: %d\n", ret);
			return ret;
		}

		spl_image->size = lzma_len;
		break;

	default:
		debug("Compression method %s is not supported\n",
		      genimg_get_comp_short_name(image_get_comp(hdr)));
		return -EINVAL;
	}

	return 0;
}
