/*
 * Copyright 2004 The Unichrome Project. All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sub license,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE UNICHROME PROJECT, AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Author: Thomas Hellstr�m 2004.
 * This code was written using docs obtained under NDA from VIA Inc.
 *
 * Don't run this code directly on an AGP buffer. Due to cache problems it will
 * be very slow.
 */


#include "via_3d_reg.h"
#include "drmP.h"

typedef enum{
	state_command,
	state_header2,
	state_header1,
	state_error
} verifier_state_t;

typedef enum{
	no_sequence = 0, 
	z_address,
	dest_address,
	tex_address
}sequence_t;


typedef enum{
	no_check = 0,
	check_for_header2,
	check_for_header1,
	check_for_header2_err,
	check_for_header1_err,
	check_for_fire,
	check_z_buffer_addr0,
	check_z_buffer_addr1,
	check_z_buffer_addr_mode,
	check_destination_addr0,
	check_destination_addr1,
	check_destination_addr_mode,
	check_for_dummy,
	check_for_dd,
	check_texture_addr0,
	check_texture_addr1,
	check_texture_addr2,
	check_texture_addr3,
	check_texture_addr4,
	check_texture_addr5,
	check_texture_addr6,
	check_texture_addr7,
	check_texture_addr8,
	check_texture_addr_mode,
	forbidden_command
}hazard_t;

/*
 * Associates each hazard above with a possible multi-command
 * sequence. For example an address that is split over multiple
 * commands and that needs to be checked at the first command 
 * that does not include any part of the address.
 */

static sequence_t seqs[] = { 
	no_sequence,
	no_sequence,
	no_sequence,
	no_sequence,
	no_sequence,
	no_sequence,
	z_address,
	z_address,
	z_address,
	dest_address,
	dest_address,
	dest_address,
	no_sequence,
	no_sequence,
	tex_address,
	tex_address,
	tex_address,
	tex_address,
	tex_address,
	tex_address,
	tex_address,
	tex_address,
	tex_address,
	tex_address,
	no_sequence
};
    
typedef struct{
	unsigned int code;
	hazard_t hz;
} hz_init_t;



static hz_init_t init_table1[] = {
	{0xf2, check_for_header2_err},
	{0xf0, check_for_header1_err},
	{0xee, check_for_fire},
	{0xcc, check_for_dummy},
	{0xdd, check_for_dd},
	{0x00, no_check},
	{0x10, check_z_buffer_addr0},
	{0x11, check_z_buffer_addr1},
	{0x12, check_z_buffer_addr_mode},
	{0x13, no_check},
	{0x14, no_check},
	{0x15, no_check},
	{0x23, no_check},
	{0x24, no_check},
	{0x33, no_check},
	{0x34, no_check},
	{0x35, no_check},
	{0x36, no_check},
	{0x37, no_check},
	{0x38, no_check},
	{0x39, no_check},
	{0x3A, no_check},
	{0x3B, no_check},
	{0x3C, no_check},
	{0x3D, no_check},
	{0x3E, no_check},
	{0x40, check_destination_addr0},
	{0x41, check_destination_addr1},
	{0x42, check_destination_addr_mode},
	{0x43, no_check},
	{0x44, no_check},
	{0x50, no_check},
	{0x51, no_check},
	{0x52, no_check},
	{0x53, no_check},
	{0x54, no_check},
	{0x55, no_check},
	{0x56, no_check},
	{0x57, no_check},
	{0x58, no_check},
	{0x70, no_check},
	{0x71, no_check},
	{0x78, no_check},
	{0x79, no_check},
	{0x7A, no_check},
	{0x7B, no_check},
	{0x7C, no_check},
	{0x7D, no_check}
};

   
		       
static hz_init_t init_table2[] = {
	{0xf2, check_for_header2_err},
	{0xf0, check_for_header1_err},
	{0xee, check_for_fire},
	{0xcc, check_for_dummy},
	{0x00, check_texture_addr0},
	{0x01, check_texture_addr0},
	{0x02, check_texture_addr0},
	{0x03, check_texture_addr0},
	{0x04, check_texture_addr0},
	{0x05, check_texture_addr0},
	{0x06, check_texture_addr0},
	{0x07, check_texture_addr0},
	{0x08, check_texture_addr0},
	{0x09, check_texture_addr0},
	{0x20, check_texture_addr1},
	{0x21, check_texture_addr1},
	{0x22, check_texture_addr1},
	{0x23, check_texture_addr4},
	{0x2B, check_texture_addr3},
	{0x2C, check_texture_addr3},
	{0x2D, check_texture_addr3},
	{0x2E, check_texture_addr3},
	{0x2F, check_texture_addr3},
	{0x30, check_texture_addr3},
	{0x31, check_texture_addr3},
	{0x32, check_texture_addr3},
	{0x33, check_texture_addr3},
	{0x34, check_texture_addr3},
	{0x4B, check_texture_addr5},
	{0x4C, check_texture_addr6},
	{0x51, check_texture_addr7},
	{0x52, check_texture_addr8},
	{0x77, check_texture_addr2},
	{0x78, no_check},
	{0x79, no_check},
	{0x7A, no_check},
	{0x7B, check_texture_addr_mode},
	{0x7C, no_check},
	{0x7D, no_check},
	{0x7E, no_check},
	{0x7F, no_check},
	{0x80, no_check},
	{0x81, no_check},
	{0x82, no_check},
	{0x83, no_check},
	{0x85, no_check},
	{0x86, no_check},
	{0x87, no_check},
	{0x88, no_check},
	{0x89, no_check},
	{0x8A, no_check},
	{0x90, no_check},
	{0x91, no_check},
	{0x92, no_check},
	{0x93, no_check}
};

static hz_init_t init_table3[] = {
	{0xf2, check_for_header2_err},
	{0xf0, check_for_header1_err},
	{0xcc, check_for_dummy},
	{0x00, no_check}
};
   
		       
static hazard_t table1[256]; 
static hazard_t table2[256]; 
static hazard_t table3[256]; 



typedef struct{
	unsigned texture;
	uint32_t z_addr; 
	uint32_t d_addr; 
        uint32_t t_addr[2][10];
	uint32_t pitch[2][10];
	uint32_t height[2][10];
	uint32_t tex_level_lo[2]; 
	uint32_t tex_level_hi[2];
	uint32_t tex_palette_size[2];
	sequence_t unfinished;
	int agp_texture;
	drm_device_t *dev;
	drm_map_t *map_cache;
} sequence_context_t;

static sequence_context_t hc_sequence;


static __inline__ int
eat_words(const uint32_t **buf, const uint32_t *buf_end, unsigned num_words)
{
	if ((*buf - buf_end) >= num_words) {
		*buf += num_words;
		return 0;
	} 
	DRM_ERROR("Illegal termination of DMA command buffer\n");
	return 1;
}


/*
 * Partially stolen from drm_memory.h
 */

static __inline__ drm_map_t *
via_drm_lookup_agp_map (sequence_context_t *seq, unsigned long offset, unsigned long size, 
			drm_device_t *dev)
{
	struct list_head *list;
	drm_map_list_t *r_list;
	drm_map_t *map = seq->map_cache;

	if (map && map->offset <= offset && (offset + size) <= (map->offset + map->size)) {
		return map;
	}
		
	list_for_each(list, &dev->maplist->head) {
		r_list = (drm_map_list_t *) list;
		map = r_list->map;
		if (!map)
			continue;
		if (map->offset <= offset && (offset + size) <= (map->offset + map->size) && 
		    !(map->flags & _DRM_RESTRICTED) && (map->type == _DRM_AGP)) {
			seq->map_cache = map;
			return map;
		}
	}
	return NULL;
}


/*
 * Require that all AGP texture levels reside in the same AGP map which should 
 * be mappable by the client. This is not a big restriction.
 * FIXME: To actually enforce this security policy strictly, drm_rmmap 
 * would have to wait for dma quiescent before removing an AGP map. 
 * The via_drm_lookup_agp_map call in reality seems to take
 * very little CPU time.
 */


static __inline__ int
finish_current_sequence(sequence_context_t *cur_seq) 
{
	switch(cur_seq->unfinished) {
	case z_address:
		DRM_DEBUG("Z Buffer start address is 0x%x\n", cur_seq->z_addr);
		break;
	case dest_address:
		DRM_DEBUG("Destination start address is 0x%x\n", cur_seq->d_addr);
		break;
	case tex_address:
		if (cur_seq->agp_texture) {			 
			unsigned start = cur_seq->tex_level_lo[cur_seq->texture];
			unsigned end = cur_seq->tex_level_hi[cur_seq->texture];
			unsigned long lo=~0, hi=0, tmp;
			uint32_t *addr, *pitch, *height, tex;
			unsigned i;

			if (end > 9) end = 9;
			if (start > 9) start = 9;

			addr =&(cur_seq->t_addr[tex = cur_seq->texture][start]);
			pitch = &(cur_seq->pitch[tex][start]);
			height = &(cur_seq->height[tex][start]);

			for (i=start; i<= end; ++i) {
				tmp = *addr++;
				if (tmp < lo) lo = tmp;
				tmp += (*height++ << *pitch++);
				if (tmp > hi) hi = tmp;
			}

			if (! via_drm_lookup_agp_map (cur_seq, lo, hi - lo, cur_seq->dev)) {
				DRM_ERROR("AGP texture is not in allowed map\n");
				return 2;
			}
		}	
		break;
	default:
		break;
	}
	cur_seq->unfinished = no_sequence;
	return 0;
}

static __inline__ int 
investigate_hazard( uint32_t cmd, hazard_t hz, sequence_context_t *cur_seq)
{
	register uint32_t tmp, *tmp_addr;

	if (cur_seq->unfinished && (cur_seq->unfinished != seqs[hz])) {
		int ret;
		if ((ret = finish_current_sequence(cur_seq))) return ret;
	}

	switch(hz) {
	case check_for_header2:
		if (cmd == HALCYON_HEADER2) return 1;
		return 0;
	case check_for_header1:
		if ((cmd & HALCYON_HEADER1MASK) == HALCYON_HEADER1) return 1;
		return 0;
	case check_for_header2_err:
		if (cmd == HALCYON_HEADER2) return 1;
		DRM_ERROR("Illegal DMA HALCYON_HEADER2 command\n");
		break;
	case check_for_header1_err:
		if ((cmd & HALCYON_HEADER1MASK) == HALCYON_HEADER1) return 1;
		DRM_ERROR("Illegal DMA HALCYON_HEADER1 command\n");
		break;
	case check_for_fire:
		if ((cmd & HALCYON_FIREMASK) == HALCYON_FIRECMD) return 1; 
		DRM_ERROR("Illegal DMA HALCYON_FIRECMD command\n");
		break;
	case check_for_dummy:
		if (HC_DUMMY == cmd) return 0;
		DRM_ERROR("Illegal DMA HC_DUMMY command\n");
		break;
	case check_for_dd:
		if (0xdddddddd == cmd) return 0;
		DRM_ERROR("Illegal DMA 0xdddddddd command\n");
		break;
	case check_z_buffer_addr0:
		cur_seq->unfinished = z_address;
		cur_seq->z_addr = (cur_seq->z_addr & 0xFF000000) |
			(cmd & 0x00FFFFFF);
		return 0;
	case check_z_buffer_addr1:
		cur_seq->unfinished = z_address;
		cur_seq->z_addr = (cur_seq->z_addr & 0x00FFFFFF) |
			((cmd & 0xFF) << 24);
		return 0;
	case check_z_buffer_addr_mode:
		cur_seq->unfinished = z_address;
		if ((cmd & 0x0000C000) == 0) return 0;
		DRM_ERROR("Attempt to place Z buffer in system memory\n");
		return 2;
	case check_destination_addr0:
		cur_seq->unfinished = dest_address;
		cur_seq->d_addr = (cur_seq->d_addr & 0xFF000000) |
			(cmd & 0x00FFFFFF);
		return 0;
	case check_destination_addr1:
		cur_seq->unfinished = dest_address;
		cur_seq->d_addr = (cur_seq->d_addr & 0x00FFFFFF) |
			((cmd & 0xFF) << 24);
		return 0;
	case check_destination_addr_mode:
		cur_seq->unfinished = dest_address;
		if ((cmd & 0x0000C000) == 0) return 0;
		DRM_ERROR("Attempt to place 3D drawing buffer in system memory\n");
		return 2;	    
	case check_texture_addr0:
		cur_seq->unfinished = tex_address;
		tmp = (cmd >> 24);
		tmp_addr = &cur_seq->t_addr[cur_seq->texture][tmp];
		*tmp_addr = (*tmp_addr & 0xFF000000) | (cmd & 0x00FFFFFF);
		return 0;
	case check_texture_addr1:
		cur_seq->unfinished = tex_address;
		tmp = ((cmd >> 24) - 0x20);
		tmp += tmp << 1;
		tmp_addr = &cur_seq->t_addr[cur_seq->texture][tmp];
		*tmp_addr = (*tmp_addr & 0x00FFFFFF) | ((cmd & 0xFF) << 24);
		tmp_addr++;
		*tmp_addr = (*tmp_addr & 0x00FFFFFF) | ((cmd & 0xFF00) << 16);
		tmp_addr++;
		*tmp_addr = (*tmp_addr & 0x00FFFFFF) | ((cmd & 0xFF0000) << 8);
		return 0;
	case check_texture_addr2:
		cur_seq->unfinished = tex_address;
		cur_seq->tex_level_lo[tmp = cur_seq->texture] = cmd & 0x3F;
		cur_seq->tex_level_hi[tmp] = (cmd & 0xFC0) >> 6;
		return 0;
	case check_texture_addr3:
		cur_seq->unfinished = tex_address;
		tmp = ((cmd >> 24) - 0x2B);
		cur_seq->pitch[cur_seq->texture][tmp] = (cmd & 0x00F00000) >> 20;
		if (!tmp && (cmd & 0x000FFFFF)) {
			DRM_ERROR("Unimplemented texture level 0 pitch mode.\n");
			return 2;
		}
		return 0;
	case check_texture_addr4:
		cur_seq->unfinished = tex_address;
		tmp_addr = &cur_seq->t_addr[cur_seq->texture][9];
		*tmp_addr = (*tmp_addr & 0x00FFFFFF) | ((cmd & 0xFF) << 24);
		return 0;
	case check_texture_addr5:
	case check_texture_addr6:
		cur_seq->unfinished = tex_address;
		/*
		 * Texture width. We don't care since we have the pitch.
		 */  
		return 0;
	case check_texture_addr7:
		cur_seq->unfinished = tex_address;
		tmp_addr = &(cur_seq->height[cur_seq->texture][0]);
		tmp_addr[5] = 1 << ((cmd & 0x00F00000) >> 20);
		tmp_addr[4] = 1 << ((cmd & 0x000F0000) >> 16);
		tmp_addr[3] = 1 << ((cmd & 0x0000F000) >> 12);
		tmp_addr[2] = 1 << ((cmd & 0x00000F00) >> 8);
		tmp_addr[1] = 1 << ((cmd & 0x000000F0) >> 4);
		tmp_addr[0] = 1 << (cmd & 0x0000000F);
		return 0;
	case check_texture_addr8:
		cur_seq->unfinished = tex_address;
		tmp_addr = &(cur_seq->height[cur_seq->texture][0]);
		tmp_addr[9] = 1 << ((cmd & 0x0000F000) >> 12);
	        tmp_addr[8] = 1 << ((cmd & 0x00000F00) >> 8);
		tmp_addr[7] = 1 << ((cmd & 0x000000F0) >> 4);
		tmp_addr[6] = 1 << (cmd & 0x0000000F);
		return 0;
	case check_texture_addr_mode:
		cur_seq->unfinished = tex_address;
		if ( 2 == (tmp = cmd & 0x00000003)) {
			DRM_ERROR("Attempt to fetch texture from system memory.\n"); 
			return 2;
		}
		cur_seq->agp_texture = (tmp == 3);
		cur_seq->tex_palette_size[cur_seq->texture] = 
			(cmd >> 16) & 0x000000007;
		return 0;
	default:
		DRM_ERROR("Illegal DMA data: 0x%x\n", cmd);
		return 2;
	}
	return 2;
}


static __inline__ verifier_state_t
via_check_header2( uint32_t const **buffer, const uint32_t *buf_end )
{
	uint32_t cmd;
	int hz_mode;
	hazard_t hz;
	const uint32_t *buf = *buffer;
	const hazard_t *hz_table;

	if ((buf_end - buf) < 2) {
		DRM_ERROR("Illegal termination of DMA HALCYON_HEADER2 sequence.\n");
		return state_error;
	}
	buf++;
	cmd = (*buf++ & 0xFFFF0000) >> 16;

	switch(cmd) {
	case HC_ParaType_CmdVdata:

		/* 
		 * Command vertex data.
		 * It is assumed that the command regulator remains in this state
		 * until it encounters a possibly double fire command or a header2 data.
		 * FIXME: Vertex data can accidently be header2 or fire.
		 * CHECK: What does the regulator do if it encounters a header1 
		 * cmd?
		 */

		while (buf < buf_end) {
			if (*buf == HALCYON_HEADER2) break;
			if ((*buf & HALCYON_FIREMASK) == HALCYON_FIRECMD) {
				buf++;
				if ((buf < buf_end) && 
				    ((*buf & HALCYON_FIREMASK) == HALCYON_FIRECMD))
					buf++;
				if ((buf < buf_end) && 
				    ((*buf & HALCYON_FIREMASK) == HALCYON_FIRECMD))
					break;
			}
			buf++;
		}
		*buffer = buf;
		return state_command;

	case HC_ParaType_NotTex:
		hz_table = table1;
		break;
	case HC_ParaType_Tex:
		hc_sequence.texture = 0;
		hz_table = table2;
		break;
	case (HC_ParaType_Tex | (HC_SubType_Tex1 << 8)):
		hc_sequence.texture = 1;
		hz_table = table2;
		break;
	case (HC_ParaType_Tex | (HC_SubType_TexGeneral << 8)):
		hz_table = table3;
		break;
	case HC_ParaType_Auto:
		if (eat_words(&buf, buf_end, 2))
			return state_error;
		*buffer = buf;
		return state_command;
	case (HC_ParaType_Palette | (HC_SubType_Stipple << 8)):
		if (eat_words(&buf, buf_end, 32))
			return state_error;
		*buffer = buf;
		return state_command;
	case (HC_ParaType_Palette | (HC_SubType_TexPalette0 << 8)):
	case (HC_ParaType_Palette | (HC_SubType_TexPalette1 << 8)):
		DRM_ERROR("Texture palettes are rejected because of "
			  "lack of info how to determine their size.\n");
		return state_error;
	case (HC_ParaType_Palette | (HC_SubType_FogTable << 8)):
		DRM_ERROR("Fog factor palettes are rejected because of "
			  "lack of info how to determine their size.\n");
		return state_error;
	default:

		/*
		 * There are some unimplemented HC_ParaTypes here, that
		 * need to be implemented if the Mesa driver is extended.
		 */

		DRM_ERROR("Invalid or unimplemented HALCYON_HEADER2 "
			  "DMA subcommand: 0x%x\n", cmd);
		*buffer = buf;
		return state_error;
	}

	while(buf < buf_end) {
		cmd = *buf++;
		if ((hz = hz_table[cmd >> 24])) {
			if ((hz_mode = investigate_hazard(cmd, hz, &hc_sequence))) {
				if (hz_mode == 1) {
					buf--;
					break;
				}
				return state_error;
			}
		} else if (hc_sequence.unfinished && 
			   finish_current_sequence(&hc_sequence)) {
			return state_error;
		}
	}
	if (hc_sequence.unfinished && finish_current_sequence(&hc_sequence)) {
		return state_error;
	}
	*buffer = buf;
	return state_command;
}


static __inline__ verifier_state_t
via_check_header1( uint32_t const **buffer, const uint32_t *buf_end )
{
	uint32_t cmd;
	const uint32_t *buf = *buffer;
	verifier_state_t ret = state_command;

	while (buf < buf_end) {
		cmd = *buf;
		if ((cmd > ((0x3FF >> 2) | HALCYON_HEADER1)) &&
		    (cmd < ((0xC00 >> 2) | HALCYON_HEADER1))) {			
			if ((cmd & HALCYON_HEADER1MASK) != HALCYON_HEADER1) 
				break;
			DRM_ERROR("Invalid HALCYON_HEADER1 command. "
				  "Attempt to access 3D- or command burst area.\n");
			ret = state_error;
			break;
		} else if (cmd > ((0xCFF >> 2) | HALCYON_HEADER1)) {
			if ((cmd & HALCYON_HEADER1MASK) != HALCYON_HEADER1) 
				break;
			DRM_ERROR("Invalid HALCYON_HEADER1 command. "
				  "Attempt to access VGA registers.\n");
			ret = state_error;
			break;			
		} else {			
			buf += 2;
		}
	}
	*buffer = buf;
	return ret;
}



int 
via_verify_command_stream(const uint32_t * buf, unsigned int size, drm_device_t *dev)
{

	uint32_t cmd;
	const uint32_t *buf_end = buf + ( size >> 2 );
	verifier_state_t state = state_command;
	
	hc_sequence.dev = dev;
	hc_sequence.unfinished = no_sequence;
	hc_sequence.map_cache = NULL;

	while (buf < buf_end) {
		switch (state) {
		case state_header2:
			state = via_check_header2( &buf, buf_end );
			break;
		case state_header1:
			state = via_check_header1( &buf, buf_end );
			break;
		case state_command:
			if (HALCYON_HEADER2 == (cmd = *buf)) 
				state = state_header2;
			else if ((cmd & HALCYON_HEADER1MASK) == HALCYON_HEADER1) 
				state = state_header1;
			else {
				DRM_ERROR("Invalid / Unimplemented DMA HEADER command. 0x%x\n",
					  cmd);
				state = state_error;
			}
			break;
		case state_error:
		default:
			return DRM_ERR(EINVAL);			
		}
	}	
	return (state == state_error) ? DRM_ERR(EINVAL) : 0;
}

static void 
setup_hazard_table(hz_init_t init_table[], hazard_t table[], int size)
{
	int i;

	for(i=0; i<256; ++i) {
		table[i] = forbidden_command;
	}

	for(i=0; i<size; ++i) {
		table[init_table[i].code] = init_table[i].hz;
	}
}

void 
via_init_command_verifier( void )
{
	hc_sequence.texture = 0;
	setup_hazard_table(init_table1, table1, sizeof(init_table1) / sizeof(hz_init_t));
	setup_hazard_table(init_table2, table2, sizeof(init_table2) / sizeof(hz_init_t));
	setup_hazard_table(init_table3, table3, sizeof(init_table3) / sizeof(hz_init_t));
}
