#include "system.h"

#ifndef THIS_IS_NOT_XBMC
  #if (defined HAVE_CONFIG_H) && (!defined WIN32)
    #include "config.h"
  #endif

  #include "DVDDemuxers/DVDDemux.h"
  #include "DVDStreamInfo.h"
  #include "DVDClock.h"
  #include "DVDVideoCodec.h"
  #include "DVDVideoCodecMFC.h"
  #include "windowing/GraphicContext.h"
  #include "DVDCodecs/DVDCodecs.h"
  #include "DVDCodecs/DVDCodecUtils.h"
  #include "DVDCodecs/DVDFactoryCodec.h"
  #include "settings/Settings.h"
  #include "settings/DisplaySettings.h"
  #include "settings/AdvancedSettings.h"
  #include "utils/log.h"
#endif

#include <sys/mman.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <dirent.h>

#ifdef CLASSNAME
#undef CLASSNAME
#endif
#define CLASSNAME "CDVDVideoCodecMFC"


#define BUFFER_SIZE        1048576 // Compressed frame size. 1080p mpeg4 10Mb/s can be >256k in size, so this is to make sure frame fits into the buffer
                                   // For very unknown reason lesser than 1Mb buffer causes MFC to corrupt its own setup, setting inapropriate values
#define INPUT_BUFFERS      3
#define OUTPUT_BUFFERS     3

#define DIRECT_RENDER_V4L2_BUFFERS 0 // set to '1' in order to directly render 4VL2 buffers, ie. do Texture Upload directly
                                     // from mmap()-ed FIMC output buffer.
                                     // NOTE: for ODROID-U3, this seems SLOW, so it seems better to memcpy the picture into
                                     // an intermediate buffer before passing it to rendering. in that case, FIMC is disabled
                                     // at all, because we do the 64x32 macroblock un-tiling of MFC's native output format in
                                     // software on-the-fly when copying the picture to the intermediate buffer.

#define memzero(x) memset(&(x), 0, sizeof (x))


union PtsReinterpreter
{
  int32_t as_int32[2];
  double as_double;
};

#if DIRECT_RENDER_V4L2_BUFFERS

#define FINAL_OUTPUT_BUFFERS (OUTPUT_BUFFERS+3) // rendering may keep up to three buffers locked in that case

#else//DIRECT_RENDER_V4L2_BUFFERS

#define FINAL_OUTPUT_BUFFERS OUTPUT_BUFFERS

// memcpy from src to dst with on-the-fly macroblock decoding
static void macroblock_memcpy_64x32(uint8_t* dst, int dst_stride, const uint8_t* src, int src_stride, int height)
{
#if defined(HAS_NEON)
  int dst_block_fix_neon = dst_stride - 64;
  bool neon_alignment_src32_dst32_met = (src_stride&31) == 0 && (dst_stride&31) == 0;
  bool neon_alignment_src32_dst16_met = (src_stride&31) == 0 && (dst_stride&15) == 0;
#endif//defined(HAS_NEON)

  // determine number of blocks in x and y direction

  int total_blocks_x = src_stride / 64;
  int total_blocks_y = (height + 31) / 32;
  int total_blocks = total_blocks_x*total_blocks_y;

  // loop through all blocks

  int z= 0, blockline= 0, blockcolumn= 0;
  const uint8_t* src_block= src;
  for (; total_blocks; --total_blocks)
  {
    // dest coordinates in picture

    int dst_x = blockcolumn * 64;
    int dst_y = blockline * 32;
    uint8_t* dst_block= dst + dst_x + dst_y*dst_stride;

    // determine max number of pixels in x/y to be copied 
    // to prevent writing out of bounds at destination

    int maxx = dst_stride - dst_x;
    if (maxx >= 64)
      maxx = 64;
    int maxy = height - dst_y;
    if (maxy >= 32)
      maxy = 32; 
    int src_block_fix = (32-maxy)*64;

    // copy block line-by-line

#if defined(HAS_NEON)
    if (maxx == 64 && neon_alignment_src32_dst32_met) {
      for (; maxy; --maxy, dst_block+=dst_block_fix_neon) {
        asm volatile (
          "vld1.64  {d0, d1, d2, d3}, [%[src_block],:256]!  \n"
          "pld      [%[src_block], #32]                      \n"
          "vst1.64  {d0, d1, d2, d3}, [%[dst_block],:256]!  \n"
          "vld1.64  {d0, d1, d2, d3}, [%[src_block],:256]!  \n"
          "pld      [%[src_block], #32]                      \n"
          "vst1.64  {d0, d1, d2, d3}, [%[dst_block],:256]!  \n"
          : [dst_block]"+r"(dst_block), [src_block]"+r"(src_block)
          :
          : "d0", "d1", "d2", "d3"
        );
      } 
    }
    else if (maxx == 64 && neon_alignment_src32_dst16_met) {
      for (; maxy; --maxy, dst_block+=dst_block_fix_neon) {
        asm volatile (
          "vld1.64  {d0, d1, d2, d3}, [%[src_block],:256]!  \n"
          "pld      [%[src_block], #32]                      \n"
          "vst1.64  {d0, d1, d2, d3}, [%[dst_block],:128]!  \n"
          "vld1.64  {d0, d1, d2, d3}, [%[src_block],:256]!  \n"
          "pld      [%[src_block], #32]                      \n"
          "vst1.64  {d0, d1, d2, d3}, [%[dst_block],:128]!  \n"
          : [dst_block]"+r"(dst_block), [src_block]"+r"(src_block)
          :
          : "d0", "d1", "d2", "d3"
        );
      } 
    }
    else
#endif//defined(HAS_NEON)
    {
      for (; maxy; --maxy, src_block+=64, dst_block+=dst_stride) 
        memcpy(dst_block, src_block, maxx);
    }
    
    src_block += src_block_fix;

    // do that voodoo Z magic
    
    if (z==1) {
      blockcolumn -= 1; 
      blockline +=1; 
      z = (z+1)&7;
      if (blockline == total_blocks_y) {
        blockcolumn += 2;
        blockline -= 1;
        z= 6; 
      }
    }
    else if (z==5) {
      blockcolumn -= 1; 
      blockline -=1; 
      z = (z+1)&7;
    }
    else {
      blockcolumn += 1; 
      z = (z+1)&7;
      if (blockcolumn == total_blocks_x) {
        switch (z) {
          case 0: 
          case 7: blockcolumn  = 0; blockline += 2; z=0; break;
          case 1: blockcolumn -= 1; blockline += 1; z=2; break; 
          case 3: 
          case 4: blockcolumn  = 0; blockline += 1; z=0; break;
          case 5: blockcolumn -= 1; blockline -= 1; z=6; break;
        }
      }
    }
  }
}

#endif//DIRECT_RENDER_V4L2_BUFFERS

CVideoBufferMFC::CVideoBufferMFC(int id) :
  CVideoBuffer(id) {
  m_v4l2buffer.iIndex = -1;
  m_width = 0;
  m_height = 0;
  for (int i=0; i<YuvImage::MAX_PLANES; ++i)
    mp_planes[i]= mp_planes_unaligned[i] = nullptr;
}

CVideoBufferMFC::~CVideoBufferMFC() {
  for (int i=0; i<YuvImage::MAX_PLANES; ++i)
    delete mp_planes_unaligned[i];
}

void CVideoBufferMFC::Set(V4l2SinkBuffer *pBuffer, int v4l2_pixelformat, int width, int height, int stride) {
  m_v4l2buffer = *pBuffer;

  // determine AV pixelformat

  AVPixelFormat av_pixformat = AV_PIX_FMT_NONE;
#if !DIRECT_RENDER_V4L2_BUFFERS
  if (v4l2_pixelformat == V4L2_PIX_FMT_NV12MT) // V4L2_PIX_FMT_NV12M in 64x32 macroblock tiles
    av_pixformat = AV_PIX_FMT_NV12;
  else 
#endif//!DIRECT_RENDER_V4L2_BUFFERS
  if (v4l2_pixelformat == V4L2_PIX_FMT_NV12M)
    av_pixformat = AV_PIX_FMT_NV12;
  else if (v4l2_pixelformat == V4L2_PIX_FMT_YUV420M)
    av_pixformat = AV_PIX_FMT_YUV420P;
  else 
      CLog::Log(LOGERROR, "%s::%s - unsupported 4vl2 format %x", CLASSNAME, __func__, v4l2_pixelformat);

#if !DIRECT_RENDER_V4L2_BUFFERS

  // if we do not directly pass on V4L2 to rendering and image format has changed,
  // then (re-)allocate temporary image buffers.

  if (av_pixformat != m_pixFormat || m_width != width || m_height != height) {

    for (int i=0; i<YuvImage::MAX_PLANES; ++i) {
      delete mp_planes_unaligned[i];
      mp_planes[i] = mp_planes_unaligned[i] = nullptr;
    }

    if (av_pixformat == AV_PIX_FMT_NV12) {
      mp_planes_unaligned[0] = new uint8_t[32 + width * height];
      mp_planes_unaligned[1] = new uint8_t[32 + width * height/2];
    }
    else if (av_pixformat == AV_PIX_FMT_YUV420P) {
      mp_planes_unaligned[0] = new uint8_t[32 + width * height];
      mp_planes_unaligned[1] = new uint8_t[32 + width/2 * height/2];
      mp_planes_unaligned[2] = new uint8_t[32 + width/2 * height/2];
    }
    else {
      CLog::Log(LOGERROR, "%s::%s - unsupported av format %d", CLASSNAME, __func__, av_pixformat);
    }

    // do 32-byte alignment of starting addresses in case NEON is in use
    mp_planes[0] = (uint8_t*)((size_t)(mp_planes_unaligned[0]+31)&~31);
    mp_planes[1] = (uint8_t*)((size_t)(mp_planes_unaligned[1]+31)&~31);
    mp_planes[2] = (uint8_t*)((size_t)(mp_planes_unaligned[2]+31)&~31);

#if defined(HAS_NEON)
    if (v4l2_pixelformat == V4L2_PIX_FMT_NV12MT && (width&15) != 0) {
      // NOTE: as a compromise, we allow 16-byte line-alignment in target image.
      // source-stride is 32-byte aligned anyway, because it is 64x32 byte
      // macroblocks.
      CLog::Log(LOGNOTICE, "%s::%s - image stride incompatible to NEON. using memcpy fallback", CLASSNAME, __func__);
    }
#endif//defined(HAS_NEON)
  }

  // copy V4L2 buffers to temporary image buffers

  uint8_t *src, *dst;
  if (v4l2_pixelformat == V4L2_PIX_FMT_NV12MT) {
    src= (uint8_t*)pBuffer->cPlane[0];
    dst= mp_planes[0];
    macroblock_memcpy_64x32(dst, width, src, stride, height);
    src= (uint8_t*)pBuffer->cPlane[1];
    dst= mp_planes[1];
    macroblock_memcpy_64x32(dst, width, src, stride, height/2);
  }  
  else if (v4l2_pixelformat == V4L2_PIX_FMT_NV12M) {
    src= (uint8_t*)pBuffer->cPlane[0];
    dst= mp_planes[0];
    for (int y=0; y<height; ++y, src+=stride, dst+=width)
      memcpy(dst, src, width);
    src= (uint8_t*)pBuffer->cPlane[1];
    dst= mp_planes[1];
    for (int y=0; y<height/2; ++y, src+=stride, dst+=width)
      memcpy(dst, src, width);
  }
  else if (v4l2_pixelformat == V4L2_PIX_FMT_YUV420M) {
    src= (uint8_t*)pBuffer->cPlane[0];
    dst= mp_planes[0];
    for (int y=0; y<height; ++y, src+=stride, dst+=width)
      memcpy(dst, src, width);
    src= (uint8_t*)pBuffer->cPlane[1];
    dst= mp_planes[1];
    for (int y=0; y<height/2; ++y, src+=stride/2, dst+=width/2)
      memcpy(dst, src, width/2);
    src= (uint8_t*)pBuffer->cPlane[2];
    dst= mp_planes[2];
    for (int y=0; y<height/2; ++y, src+=stride/2, dst+=width/2)
      memcpy(dst, src, width/2);
  }

#else //!DIRECT_RENDER_V4L2_BUFFERS

  mp_planes[0]= (uint8_t*)pBuffer->cPlane[0];
  mp_planes[1]= (uint8_t*)pBuffer->cPlane[1];
  mp_planes[2]= (uint8_t*)pBuffer->cPlane[2];

#endif//!DIRECT_RENDER_V4L2_BUFFERS

  m_pixFormat = av_pixformat;
  m_width = width;
  m_height = height;
}

void CVideoBufferMFC::GetPlanes(uint8_t*(&planes)[YuvImage::MAX_PLANES]) {
  planes[0] = mp_planes[0];
  planes[1] = mp_planes[1];
  planes[2] = mp_planes[2];
}

void CVideoBufferMFC::GetStrides(int(&strides)[YuvImage::MAX_PLANES]) {
  if (m_pixFormat == AV_PIX_FMT_YUV420P) {
    strides[0] = m_width;
    strides[1] = m_width/2;
    strides[2] = m_width/2;
  }
  else if (m_pixFormat == AV_PIX_FMT_NV12) {
    strides[0] = m_width;
    strides[1] = m_width;
    strides[2] = 0;
  }
  else {
    CLog::Log(LOGERROR, "%s::%s - unsupported format %d", CLASSNAME, __func__, m_pixFormat);
  }
}



/***************************************************************************/



CVideoBufferPoolMFC::CVideoBufferPoolMFC(CMFCCodec* pCodec) :
  mp_codec(pCodec) {
}

CVideoBufferPoolMFC::~CVideoBufferPoolMFC() {

  debug_log(LOGDEBUG, "%s::%s - deleting %d buffers", CLASSNAME, __func__, m_videoBuffers.size());
  for (auto picture : m_videoBuffers)
    delete picture;
}

CVideoBuffer* CVideoBufferPoolMFC::Get() {
  CSingleLock lock(m_criticalSection);

  if (m_freeBuffers.empty()) {
    m_freeBuffers.push_back(m_videoBuffers.size());
    m_videoBuffers.push_back(new CVideoBufferMFC(static_cast<int>(m_videoBuffers.size())));
  }
  int bufferIdx(m_freeBuffers.back());
  m_freeBuffers.pop_back();

  m_videoBuffers[bufferIdx]->Acquire(shared_from_this());
  debug_log(LOGDEBUG, "%s::%s - acquired buffer with id #%d", CLASSNAME, __func__, bufferIdx);

  return m_videoBuffers[bufferIdx];
}

void CVideoBufferPoolMFC::Return(int id) {
  CSingleLock lock(m_criticalSection);

#if DIRECT_RENDER_V4L2_BUFFERS
  if (mp_codec && (size_t)id < m_videoBuffers.size() && m_videoBuffers[id]->m_v4l2buffer.iIndex != -1) {
    debug_log(LOGDEBUG, "%s::%s - returning buffer with id #%d", CLASSNAME, __func__, id);
    mp_codec->ReturnBuffer(&m_videoBuffers[id]->m_v4l2buffer);
  }
  m_videoBuffers[id]->m_v4l2buffer.iIndex = -1;
#endif//DIRECT_RENDER_V4L2_BUFFERS

  m_freeBuffers.push_back(id);
}

void CVideoBufferPoolMFC::Detach() {
#if DIRECT_RENDER_V4L2_BUFFERS
  CSingleLock lock(m_criticalSection);

  // wait up to 0.5 sec until all buffers are released
  for (int retries = 0; m_freeBuffers.size() != m_videoBuffers.size() && retries<50; ++retries) {
    lock.Leave();
    usleep(10000);
    lock.Enter();
    if (!mp_codec)
      return;
  }

  for (auto picture : m_videoBuffers) {
    if (picture->m_v4l2buffer.iIndex != -1) {
      mp_codec->ReturnBuffer(&picture->m_v4l2buffer);
      picture->m_v4l2buffer.iIndex = -1;
      // note: do not push to freeBuffers
    }
  }
#endif//DIRECT_RENDER_V4L2_BUFFERS

  mp_codec= nullptr;
}



/***************************************************************************/



CMFCCodec::CMFCCodec() {

  m_iDecoderHandle = nullptr;
  m_iConverterHandle = nullptr;
  m_MFCOutput = nullptr;
  m_MFCCapture = nullptr;
  m_FIMCOutput = nullptr;
  m_FIMCCapture = nullptr;

  m_V4l2BufferForNextData.iIndex = -1;
  m_OutputPictures_first_free = -1;
  m_OutputPictures_first_used = -1;

  m_droppedFrames = 0;
  m_codecPts = DVD_NOPTS_VALUE;
  m_codecControlFlags = 0;

}

CMFCCodec::~CMFCCodec() {

  Dispose();

}

bool CMFCCodec::OpenDevices() {
  DIR *dir;

  if ((dir = opendir ("/sys/class/video4linux/")) != nullptr) {
    struct dirent *ent;
    while ((ent = readdir (dir)) != nullptr) {
      if (strncmp(ent->d_name, "video", 5) == 0) {
        char *p;
        char name[64];
        char devname[64];
        char sysname[64];
        char drivername[32];
        char target[1024];
        ssize_t ret;

        snprintf(sysname, 64, "/sys/class/video4linux/%s", ent->d_name);
        snprintf(name, 64, "/sys/class/video4linux/%s/name", ent->d_name);

        FILE* fp = fopen(name, "r");
        if (fgets(drivername, 32, fp) != nullptr) {
          p = strchr(drivername, '\n');
          if (p != nullptr)
            *p = '\0';
        } else {
          fclose(fp);
          continue;
        }
        fclose(fp);

        ret = readlink(sysname, target, sizeof(target));
        if (ret < 0)
          continue;
        target[ret] = '\0';
        p = strrchr(target, '/');
        if (p == nullptr)
          continue;

        sprintf(devname, "/dev/%s", ++p);

        if (!m_iDecoderHandle && strstr(drivername, "mfc") != nullptr && strstr(drivername, "dec") != nullptr) {
          int fd = open(devname, O_RDWR | O_NONBLOCK, 0);
          if (fd > -1) {
            struct v4l2_capability cap;
            memzero(cap);
            if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0)
              if (cap.capabilities & V4L2_CAP_STREAMING &&
                (cap.capabilities & V4L2_CAP_VIDEO_M2M_MPLANE ||
                (cap.capabilities & (V4L2_CAP_VIDEO_CAPTURE_MPLANE | V4L2_CAP_VIDEO_OUTPUT_MPLANE)))) {
                m_iDecoderHandle = new V4l2Device;
                m_iDecoderHandle->device = fd;
                strcpy(m_iDecoderHandle->name, drivername);
                CLog::Log(LOGDEBUG, "%s::%s - MFC Found %s %s", CLASSNAME, __func__, drivername, devname);
                struct v4l2_format fmt;
                
                memzero(fmt);
                fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
                fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12M;
                if (ioctl(fd, VIDIOC_TRY_FMT, &fmt) == 0) {
                  CLog::Log(LOGDEBUG, "%s::%s - Direct decoding to untiled picture (NV12) on device %s is supported, no conversion needed", CLASSNAME, __func__, m_iDecoderHandle->name);
                  delete m_iConverterHandle;
                  m_iConverterHandle = nullptr;
                  return true;
                }

                memzero(fmt);
                fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
                fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_YUV420M;
                if (ioctl(fd, VIDIOC_TRY_FMT, &fmt) == 0) {
                  CLog::Log(LOGDEBUG, "%s::%s - Direct decoding to untiled picture (YUV420) on device %s is supported, no conversion needed", CLASSNAME, __func__, m_iDecoderHandle->name);
                  delete m_iConverterHandle;
                  m_iConverterHandle = nullptr;
                  return true;
                }

#if !DIRECT_RENDER_V4L2_BUFFERS
                memzero(fmt);
                fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
                fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12MT;
                if (ioctl(fd, VIDIOC_TRY_FMT, &fmt) == 0) {
                  CLog::Log(LOGDEBUG, "%s::%s - Decoding to 62x32 tiled picture on device %s, disabling converter", CLASSNAME, __func__, m_iDecoderHandle->name);
                  delete m_iConverterHandle;
                  m_iConverterHandle = nullptr;
                  return true;
                }
#endif//!DIRECT_RENDER_V4L2_BUFFERS

              }
          }
          if (!m_iDecoderHandle)
            close(fd);
        }
        if (!m_iConverterHandle && strstr(drivername, "fimc") != nullptr && strstr(drivername, "m2m") != nullptr) {
          int fd = open(devname, O_RDWR | O_NONBLOCK, 0);
          if (fd > -1) {
            struct v4l2_capability cap;
            memzero(cap);
            if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0)
              if (cap.capabilities & V4L2_CAP_STREAMING &&
                (cap.capabilities & V4L2_CAP_VIDEO_M2M_MPLANE ||
                (cap.capabilities & (V4L2_CAP_VIDEO_CAPTURE_MPLANE | V4L2_CAP_VIDEO_OUTPUT_MPLANE)))) {
                m_iConverterHandle = new V4l2Device;
                m_iConverterHandle->device = fd;
                strcpy(m_iConverterHandle->name, drivername);
                CLog::Log(LOGDEBUG, "%s::%s - FIMC Found %s %s", CLASSNAME, __func__, drivername, devname);
              }
          }
          if (!m_iConverterHandle)
            close(fd);
        }
        if (m_iDecoderHandle && m_iConverterHandle) {
          closedir (dir);
          return true;
        }
      }
    }
    closedir (dir);
  }

  return false;

}

void CMFCCodec::Dispose() {

  CLog::Log(LOGNOTICE, "%s::%s", CLASSNAME, __func__);

  m_V4l2BufferForNextData.iIndex = -1;
  m_OutputPictures_first_free = -1;
  m_OutputPictures_first_used = -1;
  m_OutputPictures.clear();
  
  // detach (and cleanup) video buffer pool

  if (msp_buffer_pool) {
    msp_buffer_pool->Detach();
    msp_buffer_pool.reset();
  }

  // clean up codec

  CSingleLock lock(m_criticalSection);

  delete m_FIMCCapture;
  delete m_FIMCOutput;
  delete m_MFCCapture;
  delete m_MFCOutput;

  m_MFCOutput = nullptr;
  m_MFCCapture = nullptr;
  m_FIMCOutput = nullptr;
  m_FIMCCapture = nullptr;

  if (m_iConverterHandle) {
    close(m_iConverterHandle->device);
    delete m_iConverterHandle;
    m_iConverterHandle = nullptr;
  }

  if (m_iDecoderHandle) {
    close(m_iDecoderHandle->device);
    delete m_iDecoderHandle;
    m_iDecoderHandle = nullptr;
  }

}

bool CMFCCodec::Open(CDVDStreamInfo &hints, CDVDCodecOptions &options) {
  struct v4l2_format fmt;
  struct v4l2_crop crop;
  struct V4l2SinkBuffer sinkBuffer;
  V4l2Device *finalSink = nullptr;
  unsigned int extraSize = 0;
  uint8_t *extraData = nullptr;

  if (m_iDecoderHandle || m_iConverterHandle)
    Dispose();

  m_hints = hints;

  m_finalFormat = -1;
  m_bVideoConvert = false;
  m_codecControlFlags = 0;
  m_droppedFrames = 0;
  m_codecPts = DVD_NOPTS_VALUE;
  msp_buffer_pool.reset(new CVideoBufferPoolMFC(this));

  m_V4l2BufferForNextData.iIndex = -1;
  m_preferAddData = 3;

  if (!OpenDevices()) { 
    CLog::Log(LOGERROR, "%s::%s - No Exynos MFC Decoder/Converter found", CLASSNAME, __func__);
    return false;
  }

  m_bVideoConvert = m_converter.Open(m_hints.codec, (uint8_t *)m_hints.extradata, m_hints.extrasize, true);

  if(m_bVideoConvert) {
    if(m_converter.GetExtraData() != nullptr && m_converter.GetExtraSize() > 0) {
      extraSize = m_converter.GetExtraSize();
      extraData = m_converter.GetExtraData();
    }
  } else {
    if(m_hints.extrasize > 0 && m_hints.extradata != nullptr) {
      extraSize = m_hints.extrasize;
      extraData = (uint8_t*)m_hints.extradata;
    }
  }

  // Test what formats we can get finally
  // If converter is present, it is our final sink
  finalSink = m_iConverterHandle ? m_iConverterHandle : m_iDecoderHandle;

#if !DIRECT_RENDER_V4L2_BUFFERS
  if (!m_iConverterHandle && m_finalFormat < 0) {
    // Test 64x32 TILED NV12 2 Planes Y/CbCr 
    memzero(fmt);
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12MT;
    if (ioctl(finalSink->device, VIDIOC_TRY_FMT, &fmt) == 0) {
      m_finalFormat = V4L2_PIX_FMT_NV12MT;
      msp_buffer_pool->Configure(AV_PIX_FMT_NV12, 0);
    }
  }
#endif//!DIRECT_RENDER_V4L2_BUFFERS
  if (m_finalFormat < 0) {
    // Test NV12 2 Planes Y/CbCr 
    memzero(fmt);
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12M;
    if (ioctl(finalSink->device, VIDIOC_TRY_FMT, &fmt) == 0) {
      m_finalFormat = V4L2_PIX_FMT_NV12M;
      msp_buffer_pool->Configure(AV_PIX_FMT_NV12, 0);
    }
  }
  if (m_finalFormat < 0) {
    // Test YUV420 3 Planes Y/CbCr
    memzero(fmt);
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_YUV420M;
    if (ioctl(finalSink->device, VIDIOC_TRY_FMT, &fmt) == 0) {
      m_finalFormat = V4L2_PIX_FMT_YUV420M;
      msp_buffer_pool->Configure(AV_PIX_FMT_YUV420P, 0);
    }
  }

  // No suitable output formats available
  if (m_finalFormat < 0) {
    CLog::Log(LOGERROR, "%s::%s - No suitable format on %s to convert to found", CLASSNAME, __func__, finalSink->name);
    return false;
  }

  // Create MFC Output sink (the one where encoded frames are feed)
  m_MFCOutput = new CLinuxV4l2Sink(m_iDecoderHandle, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
  memzero(fmt);
  switch(m_hints.codec)
  {
    case AV_CODEC_ID_VC1:
      fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_VC1_ANNEX_G;
      m_name = "mfc-vc1";
      break;
    case AV_CODEC_ID_MPEG1VIDEO:
      fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_MPEG1;
      m_name = "mfc-mpeg1";
      break;
    case AV_CODEC_ID_MPEG2VIDEO:
      fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_MPEG2;
      m_name = "mfc-mpeg2";
      break;
    case AV_CODEC_ID_MPEG4:
      fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_MPEG4;
      m_name = "mfc-mpeg4";
      break;
    case AV_CODEC_ID_H263:
      fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_H263;
      m_name = "mfc-h263";
      break;
    case AV_CODEC_ID_H264:
      fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_H264;
      m_name = "mfc-h264";
      break;
    default:
      return false;
  }

  CLog::Log(LOGNOTICE, "%s::%s() using codec %s, format extrasize %d", CLASSNAME, __func__, m_name.c_str(), extraSize);

  fmt.fmt.pix_mp.plane_fmt[0].sizeimage = BUFFER_SIZE;
  // Set encoded format
  if (!m_MFCOutput->SetFormat(&fmt)) {
    CLog::Log(LOGERROR, "%s::%s setting MFCOutput format failed", CLASSNAME, __func__);
    return false;
  }
  // Init with number of input buffers predefined
  if (!m_MFCOutput->Init(INPUT_BUFFERS)) {
    CLog::Log(LOGERROR, "%s::%s MFCOutput init failed", CLASSNAME, __func__);
    return false;
  }
  // Get empty picture to fill
  if (!m_MFCOutput->GetBuffer(&sinkBuffer)) {
    CLog::Log(LOGERROR, "%s::%s cannot get output buffer", CLASSNAME, __func__);
    return false;
  }
  // Fill it with the header
  sinkBuffer.iBytesUsed[0] = extraSize;
  memcpy(sinkBuffer.cPlane[0], extraData, extraSize);
  // Enqueue picture
  if (!m_MFCOutput->PushBuffer(&sinkBuffer)) {
    CLog::Log(LOGERROR, "%s::%s cannot push stream header", CLASSNAME, __func__);
    return false;
  }
  // Create MFC Capture sink (the one from which decoded frames are read)
  m_MFCCapture = new CLinuxV4l2Sink(m_iDecoderHandle, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
  memzero(fmt);
  // If there is no converter set output format on the MFC Capture sink
  if (!m_iConverterHandle) {
    fmt.fmt.pix_mp.pixelformat = m_finalFormat;
    if (!m_MFCCapture->SetFormat(&fmt)) {
        CLog::Log(LOGERROR, "%s::%s cannot set capture format", CLASSNAME, __func__);
        return false;
    }
  }

  // Turn on MFC Output with header in it to initialize MFC with all we just setup
  m_MFCOutput->StreamOn(VIDIOC_STREAMON);

  // Initialize MFC Capture.
  // NOTE: a negative value means "allocate min buffer required + abs(val)"
  if (!m_MFCCapture->Init((!m_iConverterHandle) ? -FINAL_OUTPUT_BUFFERS : 0)) {
    CLog::Log(LOGERROR, "%s::%s MFCCapture init failed", CLASSNAME, __func__);
    return false;
  }
  // Queue all buffers (empty) to MFC Capture
  m_MFCCapture->QueueAll();

  // Read the format of MFC Capture
  if (!m_MFCCapture->GetFormat(&fmt)) {
    CLog::Log(LOGERROR, "%s::%s cannot get MFCCapture format", CLASSNAME, __func__);
    return false;
  }
  // Size of resulting picture coming out of MFC
  // It will be aligned by 16 since the picture is tiled
  // We need this to know where to split picture line by line
  m_resultLineSize = fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
  // Get MFC capture crop settings
  if (!m_MFCCapture->GetCrop(&crop)) {
    CLog::Log(LOGERROR, "%s::%s cannot get MFCCapture crop", CLASSNAME, __func__);
    return false;
  }

  // Turn on MFC Capture
  m_MFCCapture->StreamOn(VIDIOC_STREAMON);

  // If converter is needed (we need to untile the picture from format MFC produces it)
  if (m_iConverterHandle) {
    // Create FIMC Output sink
    m_FIMCOutput = new CLinuxV4l2Sink(m_iConverterHandle, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
    // Set the FIMC Output format to the one read from MFC
    if (!m_FIMCOutput->SetFormat(&fmt)) {
      CLog::Log(LOGERROR, "%s::%s setting FIMCOutput format failed", CLASSNAME, __func__);
      return false;
    }
    // Set the FIMC Output crop to the one read from MFC
    if (!m_FIMCOutput->SetCrop(&crop)) {
      CLog::Log(LOGERROR, "%s::%s setting FIMCOutput crop failed", CLASSNAME, __func__);
      return false;
    }
    // Init FIMC Output and link it to buffers of MFC Capture
    if (!m_FIMCOutput->Init(m_MFCCapture)) {
      CLog::Log(LOGERROR, "%s::%s FIMCOutput init failed", CLASSNAME, __func__);
      return false;
    }
    // Get FIMC Output crop settings
    if (!m_FIMCOutput->GetCrop(&crop)) {
      CLog::Log(LOGERROR, "%s::%s getting FIMCOutput crop failed", CLASSNAME, __func__);
      return false;
    }

    // Create FIMC Capture sink
    m_FIMCCapture = new CLinuxV4l2Sink(m_iConverterHandle, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
    // Set the final picture format and the same picture dimension settings to FIMC Capture
    // as picture crop coming from MFC (original picture dimensions)
    memzero(fmt);
    fmt.fmt.pix_mp.pixelformat = m_finalFormat;
    fmt.fmt.pix_mp.width = crop.c.width;
    fmt.fmt.pix_mp.height = crop.c.height;
    fmt.fmt.pix_mp.field = V4L2_FIELD_ANY;
    if (!m_FIMCCapture->SetFormat(&fmt)) {
      CLog::Log(LOGERROR, "%s::%s setting FIMCCapture format failed", CLASSNAME, __func__);
      return false;
    }
    // Init FIMC capture with number of buffers predefined
    if (!m_FIMCCapture->Init(FINAL_OUTPUT_BUFFERS)) {
      CLog::Log(LOGERROR, "%s::%s FIMCCapture init failed", CLASSNAME, __func__);
      return false;
    }

    // Queue all buffers (empty) to FIMC Capture
    m_FIMCCapture->QueueAll();

    // Read FIMC capture format settings
    if (!m_FIMCCapture->GetFormat(&fmt)) {
      CLog::Log(LOGERROR, "%s::%s getting FIMCCapture format failed", CLASSNAME, __func__);
      return false;
    }
    m_resultLineSize = fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
    // Read FIMC capture crop settings
    if (!m_FIMCCapture->GetCrop(&crop)) {
      CLog::Log(LOGERROR, "%s::%s getting FIMCCapture crop failed", CLASSNAME, __func__);
      return false;
    }

    // Turn on FIMC Output and Capture enabling the converter
    m_FIMCOutput->StreamOn(VIDIOC_STREAMON);
    m_FIMCCapture->StreamOn(VIDIOC_STREAMON);
  }

  // determine resulting video picture format

  m_resultFormat.color_space     = AVCOL_SPC_UNSPECIFIED;
  m_resultFormat.color_range     = AVCOL_RANGE_UNSPECIFIED;
  m_resultFormat.color_primaries = AVCOL_PRI_UNSPECIFIED;
  m_resultFormat.colorBits       = 8;

  m_resultFormat.iWidth          = crop.c.width;
  m_resultFormat.iHeight         = crop.c.height;

  m_resultFormat.iDisplayWidth   = crop.c.width;
  m_resultFormat.iDisplayHeight  = crop.c.height;

  if (m_hints.aspect > 0.0 && !m_hints.forced_aspect)
  {
    m_resultFormat.iDisplayWidth  = ((int)lrint(crop.c.height * m_hints.aspect)) & ~3;
    if (m_resultFormat.iDisplayWidth > crop.c.width)
    {
      m_resultFormat.iDisplayWidth   = crop.c.width;
      m_resultFormat.iDisplayHeight = ((int)lrint(crop.c.width / m_hints.aspect)) & ~3;
    }
  }

  m_bCodecHealthy = true;

  CLog::Log(LOGNOTICE, "%s::%s - MFC%s Setup successful (format %x, %dx%d, linesize %d)", CLASSNAME, __func__, 
    m_iConverterHandle ? "/FIMC":"", m_finalFormat, m_resultFormat.iWidth, m_resultFormat.iHeight, m_resultLineSize);
  return true;
}

void CMFCCodec::SetCodecControl(int flags) {

  if (m_codecControlFlags != flags)
  {
    debug_log(LOGDEBUG, "%s::%s - setting flags to %x", CLASSNAME, __func__, flags);
    m_codecControlFlags = flags;
  }
}

const char* CMFCCodec::GetName() { 
  return m_name.c_str(); 
}

bool CMFCCodec::AddData(const DemuxPacket &packet) {

  uint8_t* pData = packet.pData;
  int iSize = packet.iSize;
  double pts = packet.pts;

  if (m_hints.ptsinvalid)
    pts = DVD_NOPTS_VALUE;

  if(!pData)
    return false;

  PumpBuffers();

  if (!m_bCodecHealthy)
    return true;

  //unsigned int dtime = XbmcThreads::SystemClockMillis();
  debug_log(LOGDEBUG, "%s::%s - input frame iSize %d, pts %lf, dts %lf", CLASSNAME, __func__, iSize, pts, packet.dts);

  if (-1 != m_V4l2BufferForNextData.iIndex) {

    int demuxer_bytes = iSize;
    uint8_t *demuxer_content = pData;

    if(m_bVideoConvert) {
      m_converter.Convert(demuxer_content, demuxer_bytes);
      demuxer_bytes = m_converter.GetConvertSize();
      demuxer_content = m_converter.GetConvertBuffer();
    }

    debug_log(LOGDEBUG, "%s::%s - filling MFCOutput buffer %d", CLASSNAME, __func__, m_V4l2BufferForNextData.iIndex);

    memcpy((uint8_t *)m_V4l2BufferForNextData.cPlane[0], demuxer_content, demuxer_bytes);
    m_V4l2BufferForNextData.iBytesUsed[0] = demuxer_bytes;

    PtsReinterpreter ptsconv;
    ptsconv.as_double = pts;
    m_V4l2BufferForNextData.timeStamp.tv_sec = ptsconv.as_int32[0];
    m_V4l2BufferForNextData.timeStamp.tv_usec = ptsconv.as_int32[1];

    CSingleLock lock(m_criticalSection);
    if (!m_MFCOutput->PushBuffer(&m_V4l2BufferForNextData)) {
      CLog::Log(LOGERROR, "%s::%s - Cannot push buffer into MFCOutput sink", CLASSNAME, __func__);
      m_bCodecHealthy = false;
      return true; // MFC unrecoverable error, reset needed
    }
    m_V4l2BufferForNextData.iIndex = -1;
    return true;

  } else {
    CLog::Log(LOGERROR, "%s::%s - All buffers are queued and busy, no space for new frame to decode.", CLASSNAME, __func__);
    return false;
  }
}

bool CMFCCodec::GetCodecStats(double &pts, int &droppedFrames, int &skippedPics) {

  pts = m_codecPts;
  droppedFrames = m_droppedFrames;
  skippedPics = 0;
  m_droppedFrames = 0;

  return true;
}

void CMFCCodec::Reset() {

  m_droppedFrames = 0;
  m_codecPts = DVD_NOPTS_VALUE;
  m_codecControlFlags = 0;

  if (m_bCodecHealthy) {
    CLog::Log(LOGINFO, "%s::%s - Codec Reset requested, but codec is healthy, doing soft-flush", CLASSNAME, __func__);
    m_MFCOutput->SoftRestart();
    m_MFCCapture->SoftRestart();
    // give ready buffers back to V4L2
    {
      CSingleLock lock(m_criticalSection);
      while (m_OutputPictures_first_used >= 0)
      {
        int idx = m_OutputPictures_first_used;
        if (m_iConverterHandle && m_FIMCCapture)
          m_FIMCCapture->PushBuffer(&m_OutputPictures[idx]);
        else if (m_MFCCapture)
          m_MFCCapture->PushBuffer(&m_OutputPictures[idx]);

        // remove from linked list of used buffers and add to free buffers
        m_OutputPictures_first_used = m_OutputPictures[idx].m_next;
        m_OutputPictures[idx].m_next = m_OutputPictures_first_free;
        m_OutputPictures_first_free = idx;
      }
    }
  } else {
    CLog::Log(LOGERROR, "%s::%s - Codec Reset. Reinitializing", CLASSNAME, __func__);
    CDVDCodecOptions options;
    // We need full MFC/FIMC reset with device reopening.
    // I wasn't able to reinitialize both IP's without fully closing and reopening them.
    // There are always some clips that cause MFC or FIMC go into state which cannot be reset without close/open
    Open(m_hints, options);
  }
}

// if conversion is needed, pass on buffers from MFC to FIMC
void CMFCCodec::PumpBuffers() {
  
  if (!m_bCodecHealthy)
    return;

  CSingleLock lock(m_criticalSection);

  // get buffer for next data, if available

  if (-1 == m_V4l2BufferForNextData.iIndex) {
    CSingleLock lock(m_criticalSection);
    if (m_MFCOutput->GetBuffer(&m_V4l2BufferForNextData)) {
      debug_log(LOGDEBUG, "%s::%s - got empty buffer %d from MFC Output", CLASSNAME, __func__, m_V4l2BufferForNextData.iIndex);
    }
    else if (errno != EAGAIN) {
      m_bCodecHealthy= false;
      return;
    }
  }

  // get ready pictures

  bool have_new_picture;
  do
  {
    // if no buffer for storage available, then allocate a new one
    if (-1 == m_OutputPictures_first_free)
    {
      m_OutputPictures.resize(m_OutputPictures.size()+1);
      m_OutputPictures.back().m_next = -1;
      m_OutputPictures_first_free = m_OutputPictures.size()-1;
    }  
    int idx = m_OutputPictures_first_free;

    // and try to get a decodec picture
    CSingleLock lock(m_criticalSection);
    if (m_iConverterHandle)
      have_new_picture = m_FIMCCapture->DequeueBuffer(&m_OutputPictures[idx]);
    else
      have_new_picture = m_MFCCapture->DequeueBuffer(&m_OutputPictures[idx]);

    if (have_new_picture)
    {
      // if we have one, then remove buffer from linked list of empty
      // output buffers and add to used buffers
      m_OutputPictures_first_free = m_OutputPictures[idx].m_next;
      m_OutputPictures[idx].m_next = m_OutputPictures_first_used;
      m_OutputPictures_first_used = idx;
    }
  }
  while (have_new_picture);
 
  if (errno != EAGAIN) {
    CLog::Log(LOGERROR, "%s::%s - Dequeuing decoded frame failed", CLASSNAME, __func__);
    m_bCodecHealthy= false;
  }

  // if FIMC converter is in use, exchange buffers between them now
  if (!m_iConverterHandle)
    return;

  // re-queue (empty/consumed) buffers from FIMC Output to MFC Capture 

  V4l2SinkBuffer picture;
  while (m_bCodecHealthy && m_FIMCOutput->DequeueBuffer(&picture)) {
    debug_log(LOGDEBUG, "%s::%s - requeuing consumed buffer %d from FIMCOutput to MFCCapture", CLASSNAME, __func__, picture.iIndex);
    if (!m_MFCCapture->PushBuffer(&picture)) {
        CLog::Log(LOGERROR, "%s::%s - Cannot requeue picture %d from FIMC to MFC Capture", CLASSNAME, __func__, picture.iIndex);
        m_bCodecHealthy = false; // FIMC unrecoverable error, reset needed
    }
  }
  if (errno != EAGAIN)
    m_bCodecHealthy= false;

  // Get pictures from MFC Capture and pass on to FIMC Output

  while (m_bCodecHealthy && m_MFCCapture->DequeueBuffer(&picture)) {

    if (m_codecControlFlags & DVD_CODEC_CTRL_DROP) {
      debug_log(LOGDEBUG, "%s::%s - dropping frame with index %d", CLASSNAME, __func__, picture.iIndex);
      PtsReinterpreter ptsconv;
      ptsconv.as_int32[0] = picture.timeStamp.tv_sec;
      ptsconv.as_int32[1] = picture.timeStamp.tv_usec;
      m_codecPts = ptsconv.as_double;
      m_droppedFrames++;
      // directly queue it back to MFC CAPTURE for re-usage since we are in an underrun condition
      debug_log(LOGDEBUG, "%s::%s - requeuing dropped picture %d to MFCCapture", CLASSNAME, __func__, picture.iIndex);
      if (!m_MFCCapture->PushBuffer(&picture)) {
        CLog::Log(LOGERROR, "%s::%s - Cannot requeue picture %d of dropped frame to MFC Capture", CLASSNAME, __func__, picture.iIndex);
        m_bCodecHealthy = false; // FIMC unrecoverable error, reset needed
      }
    }
    // Push the picture got from MFC Capture to FIMC Output (decoded from decoder to converter)
    else {
      debug_log(LOGDEBUG, "%s::%s - passing on picture %d from MFCCapture to FIMCOutput", CLASSNAME, __func__, picture.iIndex);
      if (!m_FIMCOutput->PushBuffer(&picture)) {
        CLog::Log(LOGERROR, "%s::%s - Cannot pass on picture %d from MFC to FIMC Output", CLASSNAME, __func__, picture.iIndex);
        m_bCodecHealthy = false; // FIMC unrecoverable error, reset needed
      }
      break; // note: PushBuffer() is slow, so it seems better to leave the function after one call
    }
  }
  if (errno != EAGAIN)
    m_bCodecHealthy= false;
}

CDVDVideoCodec::VCReturn CMFCCodec::GetPicture(VideoPicture* pDvdVideoPicture) {

  pDvdVideoPicture->Reset();

  PumpBuffers();

  if (!m_bCodecHealthy)
    return CDVDVideoCodec::VC_FLUSHED;
  
  // if we returned a picture last time and a buffer for further
  // input data is free, then instruct VideoPlayer to give us more 
  // input data now
  if (m_preferAddData && -1 != m_V4l2BufferForNextData.iIndex) {
    --m_preferAddData;
    return CDVDVideoCodec::VC_BUFFER;
  }
  // otherwise, only demand for more input data, if we currently
  // don't have an output picture to return
  if (-1 == m_OutputPictures_first_used) {
    return (-1 != m_V4l2BufferForNextData.iIndex) ?
      CDVDVideoCodec::VC_BUFFER : CDVDVideoCodec::VC_NONE;
  }

  // if decoded output pictures are available, then find output picture with 
  // lowest pts value

  int *p_idx= &m_OutputPictures_first_used;
  int *p_idx_min= p_idx; 
  PtsReinterpreter pts_min;
  pts_min.as_int32[0] = m_OutputPictures[*p_idx_min].timeStamp.tv_sec;
  pts_min.as_int32[1] = m_OutputPictures[*p_idx_min].timeStamp.tv_usec;

  while ( -1 != *(p_idx = &m_OutputPictures[*p_idx].m_next))
  {
    PtsReinterpreter pts;
    pts.as_int32[0] = m_OutputPictures[*p_idx].timeStamp.tv_sec;
    pts.as_int32[1] = m_OutputPictures[*p_idx].timeStamp.tv_usec;
    
    if (pts.as_double < pts_min.as_double)
    {
      p_idx_min = p_idx;
      pts_min = pts;
    }
  }

  // remove output picture from linked list of used buffers and
  // add to list of free buffers. 
  // (p_idx_min now points to the index variable indicating the picture
  // with lowest pts)
  int idx_min= *p_idx_min;
  *p_idx_min = m_OutputPictures[idx_min].m_next;
  m_OutputPictures[idx_min].m_next = m_OutputPictures_first_free;
  m_OutputPictures_first_free = idx_min;

  // -----------------------------------------
  // HACK: ODROID-U3 specific!!!!
  // limit frame rate of 1080p to 30Hz
  //
  if (m_finalFormat == V4L2_PIX_FMT_NV12MT // should only be in use on U3
    && m_resultFormat.iWidth>=1920 && m_resultFormat.iHeight >= 1080 
    && pts_min.as_double > m_codecPts && (pts_min.as_double - m_codecPts) < 30000)
  {
    ReturnBuffer(&m_OutputPictures[idx_min]);
    m_preferAddData= 3; // next time, prefer VC_BUFFER return value
    return CDVDVideoCodec::VC_NONE;
  }
  // -----------------------------------------

  m_codecPts = pts_min.as_double;

  // now, fill *pDvdVideoPicture return value

  pDvdVideoPicture->SetParams(m_resultFormat);
  pDvdVideoPicture->videoBuffer     =  msp_buffer_pool->Get();
  static_cast<CVideoBufferMFC*>(pDvdVideoPicture->videoBuffer)
    ->Set(&m_OutputPictures[idx_min], m_finalFormat, m_resultFormat.iWidth, m_resultFormat.iHeight, m_resultLineSize);

#if !DIRECT_RENDER_V4L2_BUFFERS
  ReturnBuffer(&m_OutputPictures[idx_min]);
#endif//!DIRECT_RENDER_V4L2_BUFFERS

  pDvdVideoPicture->pts             = m_codecPts;
  pDvdVideoPicture->dts             = DVD_NOPTS_VALUE;

  pDvdVideoPicture->iFlags          = 0;
  if (m_codecControlFlags & DVD_CODEC_CTRL_DROP)
    pDvdVideoPicture->iFlags       |= DVP_FLAG_DROPPED;

  debug_log(LOGDEBUG, "%s::%s - output frame pts %lf from %s buffer %d", CLASSNAME, __func__, pDvdVideoPicture->pts, 
    m_iConverterHandle ? "FIMCCapture" : "MFCCapture",
    static_cast<CVideoBufferMFC*>(pDvdVideoPicture->videoBuffer)->m_v4l2buffer.iIndex );

  m_preferAddData= 3; // next time, prefer VC_BUFFER return value
  return CDVDVideoCodec::VC_PICTURE;
}

void CMFCCodec::ReturnBuffer(V4l2SinkBuffer* pBuffer) {
  bool success = false;
  {
    CSingleLock lock(m_criticalSection);
    if (m_iConverterHandle && m_FIMCCapture) {
      debug_log(LOGDEBUG, "%s::%s - returning buffer %d to FIMCCapture", CLASSNAME, __func__, pBuffer->iIndex);
      success= m_FIMCCapture->PushBuffer(pBuffer);
    }
    else if (m_MFCCapture) {
      debug_log(LOGDEBUG, "%s::%s - returning buffer %d to MFCCapture", CLASSNAME, __func__, pBuffer->iIndex);
      success= m_MFCCapture->PushBuffer(pBuffer);
    }
  }
  if (!success) {
    CLog::Log(LOGERROR, "%s::%s - Error returning buffer %d", CLASSNAME, __func__, pBuffer->iIndex);
    m_bCodecHealthy = false; // FIMC unrecoverable error, reset needed
  }
  else {
    pBuffer->iIndex = -1;
  }
}


/***************************************************************************/



CDVDVideoCodecMFC::CDVDVideoCodecMFC(CProcessInfo &processInfo) : CDVDVideoCodec(processInfo) {

  msp_codec.reset(new CMFCCodec());
}

CDVDVideoCodec* CDVDVideoCodecMFC::Create(CProcessInfo &processInfo)
{
  return new CDVDVideoCodecMFC(processInfo);
}

bool CDVDVideoCodecMFC::Register()
{
  CDVDFactoryCodec::RegisterHWVideoCodec("mfc", &CDVDVideoCodecMFC::Create);
  return true;
}

bool CDVDVideoCodecMFC::Open(CDVDStreamInfo &hints, CDVDCodecOptions &options) {

  if (msp_codec->Open(hints,options)) {

    m_processInfo.SetVideoDecoderName(msp_codec->GetName(), true);
    m_processInfo.SetVideoDimensions(msp_codec->m_resultFormat.iWidth, msp_codec->m_resultFormat.iHeight);
    m_processInfo.SetVideoDeintMethod("hardware");
    m_processInfo.SetVideoPixelFormat("YUV 4:2:0");
    return true;
  }
  return false;
}

void CDVDVideoCodecMFC::SetCodecControl(int flags) {
  msp_codec->SetCodecControl(flags);
}

const char* CDVDVideoCodecMFC::GetName() {
  return msp_codec->GetName();
}

bool CDVDVideoCodecMFC::AddData(const DemuxPacket &packet) {
  return msp_codec->AddData(packet);
}

bool CDVDVideoCodecMFC::GetCodecStats(double &pts, int &droppedFrames, int &skippedPics) {
  return msp_codec->GetCodecStats(pts,droppedFrames,skippedPics);
}

void CDVDVideoCodecMFC::Reset() {
  msp_codec->Reset();
}

void CDVDVideoCodecMFC::Reopen() {
  msp_codec->Reset(); // TODO: ReOpen not implemented
}

CDVDVideoCodec::VCReturn CDVDVideoCodecMFC::GetPicture(VideoPicture* pDvdVideoPicture) {
  return msp_codec->GetPicture(pDvdVideoPicture);
}
