#pragma once

#ifndef THIS_IS_NOT_XBMC
  #include "DVDVideoCodec.h"
  #include "DVDStreamInfo.h"
  #include "utils/BitstreamConverter.h"
  #include "platform/linux/LinuxV4l2Sink.h"
  #include "cores/VideoPlayer/Process/VideoBuffer.h"
#else
  #include "xbmcstubs.h"
  #include "LinuxV4l2Sink.h"
#endif

#include <vector>

class CMFCCodec;
class CDVDVideoCodecMFC;


class CVideoBufferMFC : public CVideoBuffer {
public:
  explicit CVideoBufferMFC(int id);
  virtual ~CVideoBufferMFC();
  virtual void GetPlanes(uint8_t*(&planes)[YuvImage::MAX_PLANES]) override;
  virtual void GetStrides(int(&strides)[YuvImage::MAX_PLANES]) override;

  void Set(V4l2SinkBuffer *pBuffer, int v4l2_pixelformat, int width, int height, int stride);

  V4l2SinkBuffer m_v4l2buffer;
  int m_width;
  int m_height;

  uint8_t* mp_planes_unaligned[YuvImage::MAX_PLANES];
  uint8_t* mp_planes[YuvImage::MAX_PLANES];
};


class CVideoBufferPoolMFC : public IVideoBufferPool {
public:
  explicit CVideoBufferPoolMFC(CMFCCodec* pCodec);
  virtual ~CVideoBufferPoolMFC();

  virtual CVideoBuffer* Get() override;
  virtual void Return(int id) override;

  void Detach();

  // NOTE: never lock this lock after the one in CMFCCodec to prevent deadlock conditions
  CCriticalSection m_criticalSection;
  std::vector<CVideoBufferMFC*> m_videoBuffers;
  std::vector<int> m_freeBuffers;

private:
  CMFCCodec* mp_codec;
};


class CMFCCodec
{
public:
  CMFCCodec();
  virtual ~CMFCCodec();

  bool Open(CDVDStreamInfo &hints, CDVDCodecOptions &options);

  bool AddData(const DemuxPacket &packet);
  void Reset();
  CDVDVideoCodec::VCReturn GetPicture(VideoPicture* pDvdVideoPicture);
  void ReturnBuffer(V4l2SinkBuffer* pBuffer);
  void SetCodecControl(int flags);
  const char* GetName();
  bool GetCodecStats(double &pts, int &droppedFrames, int &skippedPics);

  void Dispose();
  bool OpenDevices();
  void PumpBuffers();

  std::string m_name;

  bool m_bCodecHealthy;

  V4l2Device *m_iDecoderHandle;
  V4l2Device *m_iConverterHandle;

  CLinuxV4l2Sink *m_MFCCapture;
  CLinuxV4l2Sink *m_MFCOutput;
  CLinuxV4l2Sink *m_FIMCCapture;
  CLinuxV4l2Sink *m_FIMCOutput;

  int m_finalFormat;

  V4l2SinkBuffer  m_V4l2BufferForNextData;

  bool m_bVideoConvert;
  CDVDStreamInfo m_hints;
  VideoPicture m_resultFormat;
  int m_resultLineSize;

  CBitstreamConverter m_converter;
  int m_codecControlFlags;

  int m_droppedFrames;
  double m_codecPts;
  int m_preferAddData;

  template<typename T>
  struct Linked : T
  { 
    int m_next; ///<index of next free/used buffer 
  };
  int m_OutputPictures_first_free;///<index of first free buffer in m_OutputPictures
  int m_OutputPictures_first_used;///<index of first used buffer in m_OutputPictures
  std::vector<Linked<V4l2SinkBuffer>> m_OutputPictures;
  
  CCriticalSection m_criticalSection;
  std::shared_ptr<CVideoBufferPoolMFC> msp_buffer_pool;
};


class CDVDVideoCodecMFC : public CDVDVideoCodec
{
public:
  explicit CDVDVideoCodecMFC(CProcessInfo &processInfo);

  static CDVDVideoCodec* Create(CProcessInfo &processInfo);
  static bool Register();

  virtual bool Open(CDVDStreamInfo &hints, CDVDCodecOptions &options) override;

  virtual bool AddData(const DemuxPacket &packet) override;
  virtual void Reset() override;
  virtual void Reopen() override;
  virtual VCReturn GetPicture(VideoPicture* pDvdVideoPicture) override;
  virtual void SetCodecControl(int flags) override;
  virtual const char* GetName() override;
  virtual bool GetCodecStats(double &pts, int &droppedFrames, int &skippedPics) override;

  std::shared_ptr<CMFCCodec> msp_codec;
};
