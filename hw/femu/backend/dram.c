#include <openssl/evp.h>

#include "../nvme.h"

/* Coperd: FEMU Memory Backend (mbe) for emulated SSD */

int init_dram_backend(SsdDramBackend **mbe, int64_t nbytes) {
    SsdDramBackend *b = *mbe = g_malloc0(sizeof(SsdDramBackend));

    b->size = nbytes;
    b->logical_space = g_malloc0(nbytes);

    if (mlock(b->logical_space, nbytes) == -1) {
        femu_err("Failed to pin the memory backend to the host DRAM\n");
        g_free(b->logical_space);
        abort();
    }

    return 0;
}

void free_dram_backend(SsdDramBackend *b) {
    if (b->logical_space) {
        munlock(b->logical_space, b->size);
        g_free(b->logical_space);
    }
}

int backend_rw(SsdDramBackend *b, QEMUSGList *qsg, uint64_t *lbal, bool is_write) {
    int sg_cur_index = 0;
    dma_addr_t sg_cur_byte = 0;
    dma_addr_t cur_addr, cur_len;
    uint64_t mb_oft = lbal[0];
    void *mb = b->logical_space;

    DMADirection dir = DMA_DIRECTION_FROM_DEVICE;

    if (is_write) {
        dir = DMA_DIRECTION_TO_DEVICE;
    }

    while (sg_cur_index < qsg->nsg) {
        cur_addr = qsg->sg[sg_cur_index].base + sg_cur_byte;
        cur_len = qsg->sg[sg_cur_index].len - sg_cur_byte;
        if (dma_memory_rw(qsg->as, cur_addr, mb + mb_oft, cur_len, dir, MEMTXATTRS_UNSPECIFIED)) {
            femu_err("dma_memory_rw error\n");
        }

        sg_cur_byte += cur_len;
        if (sg_cur_byte == qsg->sg[sg_cur_index].len) {
            sg_cur_byte = 0;
            ++sg_cur_index;
        }

        if (b->femu_mode == FEMU_OCSSD_MODE) {
            mb_oft = lbal[sg_cur_index];
        } else if (b->femu_mode == FEMU_BBSSD_MODE || b->femu_mode == FEMU_NOSSD_MODE || b->femu_mode == FEMU_ZNSSD_MODE) {
            mb_oft += cur_len;
        } else {
            assert(0);
        }
    }

    qemu_sglist_destroy(qsg);

    return 0;
}

int hashing_backend_rw(SsdDramBackend *b, QEMUSGList *qsg, uint64_t *lbal, bool is_write, uint16_t nlb) {
    int sg_cur_index = 0;
    dma_addr_t sg_cur_byte = 0;
    dma_addr_t cur_addr, cur_len;
    uint64_t mb_oft = lbal[0];
    void *mb = b->logical_space;

    DMADirection dir = DMA_DIRECTION_FROM_DEVICE;

    if (is_write) {
        dir = DMA_DIRECTION_TO_DEVICE;
    }

    EVP_MD_CTX *mdctx = NULL;

    int num_pages = nlb / 8;
    qsg->num_pages = num_pages;
    qsg->is_written = 987654321;

    // should change this to what you used
    qsg->numbit = CUR_HASH_SIZE;  // sha256

    qsg->hash_array = (unsigned char **)malloc(num_pages * sizeof(unsigned char *));
    qsg->hash_len_array = (unsigned int *)malloc(num_pages * sizeof(unsigned int));
    for (int i = 0; i < num_pages; i++) qsg->hash_array[i] = (unsigned char *)malloc(EVP_MAX_MD_SIZE * sizeof(unsigned char));

    const uint64_t page_size = 4096;
    uint64_t remaining_page_size = page_size;
    int page_count = 0;
    if (is_write) {
        mdctx = EVP_MD_CTX_new();
        // should change this to what you used
        if (EVP_DigestInit_ex(mdctx, EVP_sha1(), NULL) != 1) {
            femu_err("EVP_DigestInit_ex error\n");
            EVP_MD_CTX_free(mdctx);
            return -1;
        }

        while (sg_cur_index < qsg->nsg) {
            cur_addr = qsg->sg[sg_cur_index].base + sg_cur_byte;
            cur_len = qsg->sg[sg_cur_index].len - sg_cur_byte;

            // 현재 남은 페이지 크기보다 큰 데이터 길이는 자른다
            if (cur_len > remaining_page_size) {
                cur_len = remaining_page_size;  // 남은 페이지 크기만큼만 읽는다
            }
            if (dma_memory_rw(qsg->as, cur_addr, mb + mb_oft, cur_len, dir, MEMTXATTRS_UNSPECIFIED)) {
                femu_err("dma_memory_rw error\n");
            }

            // 해시 업데이트
            if (EVP_DigestUpdate(mdctx, mb + mb_oft, cur_len) != 1) {
                femu_err("EVP_DigestUpdate error\n");
                EVP_MD_CTX_free(mdctx);
                return -1;
            }

            sg_cur_byte += cur_len;
            remaining_page_size -= cur_len;  // 남은 페이지 크기 업데이트

            if (sg_cur_byte == qsg->sg[sg_cur_index].len) {
                sg_cur_byte = 0;
                ++sg_cur_index;
            }

            if (b->femu_mode == FEMU_OCSSD_MODE) {
                mb_oft = lbal[sg_cur_index];
            } else if (b->femu_mode == FEMU_BBSSD_MODE || b->femu_mode == FEMU_NOSSD_MODE || b->femu_mode == FEMU_ZNSSD_MODE) {
                mb_oft += cur_len;
            } else {
                assert(0);
            }

            // 한 페이지를 모두 읽은 경우
            if (remaining_page_size == 0) {
                unsigned int hash_len = 0;
                if (EVP_DigestFinal_ex(mdctx, qsg->hash_array[page_count], &hash_len) != 1) {
                    femu_err("EVP_DigestFinal_ex error\n");
                    EVP_MD_CTX_free(mdctx);
                    return -1;
                }

                qsg->hash_len_array[page_count] = hash_len;
                page_count++;

                remaining_page_size = page_size;  // 새 페이지 크기 초기화

                if (EVP_DigestInit_ex(mdctx, EVP_sha256(), NULL) != 1) {
                    femu_err("EVP_DigestInit_ex error\n");
                    EVP_MD_CTX_free(mdctx);
                    return -1;
                }
            }
        }
        EVP_MD_CTX_free(mdctx);

    } else {
        while (sg_cur_index < qsg->nsg) {
            cur_addr = qsg->sg[sg_cur_index].base + sg_cur_byte;
            cur_len = qsg->sg[sg_cur_index].len - sg_cur_byte;
            if (dma_memory_rw(qsg->as, cur_addr, mb + mb_oft, cur_len, dir, MEMTXATTRS_UNSPECIFIED)) {
                femu_err("dma_memory_rw error\n");
            }

            sg_cur_byte += cur_len;
            if (sg_cur_byte == qsg->sg[sg_cur_index].len) {
                sg_cur_byte = 0;
                ++sg_cur_index;
            }

            if (b->femu_mode == FEMU_OCSSD_MODE) {
                mb_oft = lbal[sg_cur_index];
            } else if (b->femu_mode == FEMU_BBSSD_MODE || b->femu_mode == FEMU_NOSSD_MODE || b->femu_mode == FEMU_ZNSSD_MODE) {
                mb_oft += cur_len;
            } else {
                assert(0);
            }
        }
    }
    // qemu_sglist_destroy(qsg);

    return 0;
}