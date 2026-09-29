#define COBJMACROS
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <windows.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _MSC_VER
#pragma warning(push, 0)
#pragma warning(disable : 4505 5045)
#endif
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include "sim_core.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxgi.lib")

typedef struct gpu_frame_constants {
  uint32_t frame_index;
  uint32_t simulation_size[2];
  uint32_t selected_pixel;
  uint32_t viewport_origin[2];
  uint32_t viewport_size[2];
  uint32_t brush_origin[2];
  uint32_t brush_extent[2];
  uint32_t horizontal_phase;
  uint32_t frame_padding[3];
} gpu_frame_constants;

_Static_assert(sizeof(gpu_frame_constants) == 64, "gpu_frame_constants must match the HLSL Frame cbuffer");
_Static_assert(SIM_WIDTH <= 640, "the shared row solver supports at most 640 cells");
_Static_assert(SIM_PASSES_PER_UPDATE == 5u,
               "the simulation dispatch schedule contains five passes");

#define FONT_ATLAS_SIZE 512
#define FONT_FIRST_CHARACTER 32
#define FONT_CHARACTER_COUNT 95
#define TEXT_MAX_VERTICES 4096

typedef struct text_vertex {
  float position[2];
  float uv[2];
  float color[4];
} text_vertex;

typedef struct text_renderer {
  ID3D11Texture2D* atlas;
  ID3D11ShaderResourceView* atlas_srv;
  ID3D11Buffer* vertices;
  ID3D11VertexShader* vertex_shader;
  ID3D11PixelShader* pixel_shader;
  ID3D11InputLayout* input_layout;
  ID3D11SamplerState* sampler;
  ID3D11BlendState* blend;
  stbtt_bakedchar characters[FONT_CHARACTER_COUNT];
} text_renderer;

typedef struct app {
  HWND window;
  ID3D11Device* device;
  ID3D11DeviceContext* context;
  IDXGISwapChain* swap_chain;
  ID3D11RenderTargetView* backbuffer;
  ID3D11Texture2D* state[2];
  ID3D11ShaderResourceView* state_srv[2];
  ID3D11UnorderedAccessView* state_uav[2];
  ID3D11Buffer* brush_buffer;
  ID3D11ShaderResourceView* brush_srv;
  ID3D11Buffer* constants;
  ID3D11ComputeShader* simulate_shader;
  ID3D11ComputeShader* falling_vertical_shader;
  ID3D11ComputeShader* liquid_horizontal_shader;
  ID3D11ComputeShader* brush_shader;
  ID3D11VertexShader* vertex_shader;
  ID3D11PixelShader* pixel_shader;
  ID3D11PixelShader* bloom_emission_shader;
  ID3D11PixelShader* bloom_blur_horizontal_shader;
  ID3D11PixelShader* bloom_blur_vertical_shader;
  ID3D11PixelShader* bloom_composite_shader;
  ID3D11SamplerState* point_sampler;
  ID3D11SamplerState* linear_sampler;
  ID3D11Texture2D* scene_texture;
  ID3D11RenderTargetView* scene_target;
  ID3D11ShaderResourceView* scene_srv;
  ID3D11Texture2D* bloom_texture[2];
  ID3D11RenderTargetView* bloom_target[2];
  ID3D11ShaderResourceView* bloom_srv[2];
  text_renderer text;
  uint32_t read_state;
  uint32_t frame_index;
  uint32_t horizontal_phase;
  uint32_t fps;
  uint32_t fps_frames;
  LARGE_INTEGER fps_start;
  LARGE_INTEGER fps_frequency;
  LARGE_INTEGER simulation_last;
  uint64_t simulation_accumulator;
  sim_viewport viewport;
  sim_brush_queue brush_queue;
  sim_brush_bounds brush_bounds;
  sim_pixel_type selected;
  sim_pixel_type hovered;
  int mouse_x;
  int mouse_y;
  int client_width;
  int client_height;
  bool painting;
  bool tracking_mouse;
} app;

static void Release(void* object) {
  if (object != NULL)
    IUnknown_Release((IUnknown*)object);
}

static bool Check(HRESULT result, const char* where) {
  if (FAILED(result)) {
    char message[128];
    snprintf(message, sizeof(message), "%s failed: 0x%08lx", where, (unsigned long)result);
    MessageBoxA(NULL, message, "PixelSim", MB_ICONERROR);
    return false;
  }
  return true;
}

static bool CompileShader(const char* entry, const char* target, ID3DBlob** blob) {
  ID3DBlob* errors = NULL;
  HRESULT result = D3DCompileFromFile(L"assets/shaders/pixelsim.hlsl", NULL,
                                      D3D_COMPILE_STANDARD_FILE_INCLUDE, entry, target,
                                      D3DCOMPILE_ENABLE_STRICTNESS, 0, blob, &errors);
  if (FAILED(result)) {
    const char* text = errors ? (const char*)ID3D10Blob_GetBufferPointer(errors) : "shader file not found";
    fprintf(stderr, "shader %s compilation failed:\n%s\n", entry, text);
    MessageBoxA(NULL, text, "PixelSim shader compilation", MB_ICONERROR);
    Release(errors);
    return false;
  }
  Release(errors);
  return true;
}

static bool LoadUiFont(uint8_t** font_data) {
  char windows_directory[MAX_PATH];
  char font_path[MAX_PATH];
  if (!GetWindowsDirectoryA(windows_directory, MAX_PATH))
    return false;
  const int path_length = snprintf(font_path, sizeof(font_path),
                                   "%s\\Fonts\\segoeui.ttf",
                                   windows_directory);
  if (path_length < 0 || path_length >= (int)sizeof(font_path))
    return false;

  FILE* file = NULL;
  if (fopen_s(&file, font_path, "rb") != 0 || !file)
    return false;
  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    return false;
  }
  const long size = ftell(file);
  if (size <= 0 || fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    return false;
  }

  *font_data = malloc((size_t)size);
  if (!*font_data || fread(*font_data, 1, (size_t)size, file) != (size_t)size) {
    free(*font_data);
    *font_data = NULL;
    fclose(file);
    return false;
  }
  fclose(file);
  return true;
}

static bool CreateTextResources(app* application) {
  uint8_t* font_data = NULL;
  uint8_t* atlas_data = calloc(FONT_ATLAS_SIZE, FONT_ATLAS_SIZE);
  if (!atlas_data || !LoadUiFont(&font_data)) {
    free(atlas_data);
    free(font_data);
    MessageBoxA(NULL, "Could not load the Windows UI font.", "PixelSim",
                MB_ICONERROR);
    return false;
  }
  if (stbtt_BakeFontBitmap(font_data, 0, 18.0f, atlas_data,
                           FONT_ATLAS_SIZE, FONT_ATLAS_SIZE,
                           FONT_FIRST_CHARACTER, FONT_CHARACTER_COUNT,
                           application->text.characters) <= 0) {
    free(atlas_data);
    free(font_data);
    MessageBoxA(NULL, "Could not build the text atlas.", "PixelSim",
                MB_ICONERROR);
    return false;
  }
  free(font_data);
  atlas_data[0] = 255;

  D3D11_TEXTURE2D_DESC atlas = {0};
  atlas.Width = FONT_ATLAS_SIZE;
  atlas.Height = FONT_ATLAS_SIZE;
  atlas.MipLevels = 1;
  atlas.ArraySize = 1;
  atlas.Format = DXGI_FORMAT_R8_UNORM;
  atlas.SampleDesc.Count = 1;
  atlas.Usage = D3D11_USAGE_IMMUTABLE;
  atlas.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA atlas_initial = {atlas_data, FONT_ATLAS_SIZE, 0};
  const HRESULT atlas_result = ID3D11Device_CreateTexture2D(
      application->device, &atlas, &atlas_initial, &application->text.atlas);
  free(atlas_data);
  if (!Check(atlas_result, "CreateTexture2D font atlas") ||
      !Check(ID3D11Device_CreateShaderResourceView(
                 application->device,
                 (ID3D11Resource*)application->text.atlas, NULL,
                 &application->text.atlas_srv),
             "CreateShaderResourceView font atlas"))
    return false;

  D3D11_BUFFER_DESC vertices = {0};
  vertices.ByteWidth = sizeof(text_vertex) * TEXT_MAX_VERTICES;
  vertices.Usage = D3D11_USAGE_DYNAMIC;
  vertices.BindFlags = D3D11_BIND_VERTEX_BUFFER;
  vertices.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  if (!Check(ID3D11Device_CreateBuffer(application->device, &vertices, NULL,
                                       &application->text.vertices),
             "CreateBuffer text vertices"))
    return false;

  ID3DBlob* blob = NULL;
  if (!CompileShader("TextVertex", "vs_5_0", &blob))
    return false;
  bool created = Check(ID3D11Device_CreateVertexShader(
                           application->device,
                           ID3D10Blob_GetBufferPointer(blob),
                           ID3D10Blob_GetBufferSize(blob), NULL,
                           &application->text.vertex_shader),
                       "CreateVertexShader text");
  D3D11_INPUT_ELEMENT_DESC layout[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0,
       offsetof(text_vertex, position), D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0,
       offsetof(text_vertex, uv), D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,
       offsetof(text_vertex, color), D3D11_INPUT_PER_VERTEX_DATA, 0},
  };
  if (created)
    created = Check(ID3D11Device_CreateInputLayout(
                        application->device, layout,
                        sizeof(layout) / sizeof(layout[0]),
                        ID3D10Blob_GetBufferPointer(blob),
                        ID3D10Blob_GetBufferSize(blob),
                        &application->text.input_layout),
                    "CreateInputLayout text");
  Release(blob);
  blob = NULL;
  if (!created || !CompileShader("DrawText", "ps_5_0", &blob))
    return false;
  created = Check(ID3D11Device_CreatePixelShader(
                      application->device,
                      ID3D10Blob_GetBufferPointer(blob),
                      ID3D10Blob_GetBufferSize(blob), NULL,
                      &application->text.pixel_shader),
                  "CreatePixelShader text");
  Release(blob);
  if (!created)
    return false;

  D3D11_SAMPLER_DESC sampler = {0};
  sampler.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
  sampler.AddressU = sampler.AddressV = sampler.AddressW =
      D3D11_TEXTURE_ADDRESS_CLAMP;
  if (!Check(ID3D11Device_CreateSamplerState(
                 application->device, &sampler, &application->text.sampler),
             "CreateSamplerState text"))
    return false;

  D3D11_BLEND_DESC blend = {0};
  blend.RenderTarget[0].BlendEnable = TRUE;
  blend.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
  blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
  blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
  blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
  blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
  blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
  blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  return Check(ID3D11Device_CreateBlendState(
                   application->device, &blend, &application->text.blend),
               "CreateBlendState text");
}

static bool CreateStateTexture(app* application, uint32_t index) {
  D3D11_TEXTURE2D_DESC texture = {0};
  texture.Width = SIM_WIDTH;
  texture.Height = SIM_HEIGHT;
  texture.MipLevels = 1;
  texture.ArraySize = 1;
  texture.Format = DXGI_FORMAT_R32_UINT;
  texture.SampleDesc.Count = 1;
  texture.Usage = D3D11_USAGE_DEFAULT;
  texture.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
  return Check(ID3D11Device_CreateTexture2D(application->device, &texture, NULL, &application->state[index]), "CreateTexture2D") &&
         Check(ID3D11Device_CreateShaderResourceView(application->device, (ID3D11Resource*)application->state[index], NULL, &application->state_srv[index]), "CreateShaderResourceView") &&
         Check(ID3D11Device_CreateUnorderedAccessView(application->device, (ID3D11Resource*)application->state[index], NULL, &application->state_uav[index]), "CreateUnorderedAccessView");
}

static void ReleasePostProcessTargets(app* application) {
  for (uint32_t index = 0; index < 2; ++index) {
    Release(application->bloom_srv[index]);
    Release(application->bloom_target[index]);
    Release(application->bloom_texture[index]);
    application->bloom_srv[index] = NULL;
    application->bloom_target[index] = NULL;
    application->bloom_texture[index] = NULL;
  }
  Release(application->scene_srv);
  Release(application->scene_target);
  Release(application->scene_texture);
  application->scene_srv = NULL;
  application->scene_target = NULL;
  application->scene_texture = NULL;
}

static bool CreatePostProcessTexture(app* application,
                                     uint32_t width,
                                     uint32_t height,
                                     ID3D11Texture2D** texture,
                                     ID3D11RenderTargetView** target,
                                     ID3D11ShaderResourceView** resource) {
  D3D11_TEXTURE2D_DESC description = {0};
  description.Width = width;
  description.Height = height;
  description.MipLevels = 1;
  description.ArraySize = 1;
  description.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
  description.SampleDesc.Count = 1;
  description.Usage = D3D11_USAGE_DEFAULT;
  description.BindFlags =
      D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  return Check(ID3D11Device_CreateTexture2D(application->device, &description,
                                            NULL, texture),
               "CreateTexture2D post process") &&
         Check(ID3D11Device_CreateRenderTargetView(
                   application->device, (ID3D11Resource*)*texture, NULL,
                   target),
               "CreateRenderTargetView post process") &&
         Check(ID3D11Device_CreateShaderResourceView(
                   application->device, (ID3D11Resource*)*texture, NULL,
                   resource),
               "CreateShaderResourceView post process");
}

static bool CreatePostProcessTargets(app* application, int width, int height) {
  ReleasePostProcessTargets(application);
  const uint32_t bloom_width = (uint32_t)(width > 1 ? width / 2 : 1);
  const uint32_t bloom_height = (uint32_t)(height > 1 ? height / 2 : 1);
  if (!CreatePostProcessTexture(
          application, (uint32_t)width, (uint32_t)height,
          &application->scene_texture, &application->scene_target,
          &application->scene_srv))
    return false;
  for (uint32_t index = 0; index < 2; ++index) {
    if (!CreatePostProcessTexture(
            application, bloom_width, bloom_height,
            &application->bloom_texture[index],
            &application->bloom_target[index],
            &application->bloom_srv[index]))
      return false;
  }
  return true;
}

static bool CreateGpuResources(app* application) {
  D3D11_BUFFER_DESC constants = {0};
  constants.ByteWidth = sizeof(gpu_frame_constants);
  constants.Usage = D3D11_USAGE_DYNAMIC;
  constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  constants.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  if (!Check(ID3D11Device_CreateBuffer(application->device, &constants, NULL, &application->constants), "CreateBuffer constants"))
    return false;

  D3D11_BUFFER_DESC brush = {0};
  brush.ByteWidth = sizeof(sim_brush_command);
  brush.Usage = D3D11_USAGE_DYNAMIC;
  brush.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  brush.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  brush.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
  brush.StructureByteStride = sizeof(sim_brush_command);
  if (!Check(ID3D11Device_CreateBuffer(application->device, &brush, NULL, &application->brush_buffer), "CreateBuffer brush"))
    return false;
  if (!Check(ID3D11Device_CreateShaderResourceView(application->device, (ID3D11Resource*)application->brush_buffer, NULL, &application->brush_srv), "CreateShaderResourceView brush"))
    return false;

  D3D11_SAMPLER_DESC sampler = {0};
  sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
  sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  if (!Check(ID3D11Device_CreateSamplerState(application->device, &sampler, &application->point_sampler), "CreateSamplerState"))
    return false;

  sampler.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
  if (!Check(ID3D11Device_CreateSamplerState(
                 application->device, &sampler,
                 &application->linear_sampler),
             "CreateSamplerState bloom"))
    return false;

  ID3DBlob* blob = NULL;
  if (!CompileShader("Simulate", "cs_5_0", &blob))
    return false;
  bool created = Check(ID3D11Device_CreateComputeShader(application->device, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), NULL, &application->simulate_shader), "CreateComputeShader");
  Release(blob);
  blob = NULL;
  if (!created ||
      !CompileShader("MoveFallingMaterialsVertical", "cs_5_0", &blob))
    return false;
  created = Check(ID3D11Device_CreateComputeShader(
                      application->device,
                      ID3D10Blob_GetBufferPointer(blob),
                      ID3D10Blob_GetBufferSize(blob), NULL,
                      &application->falling_vertical_shader),
                  "CreateComputeShader falling materials vertical");
  Release(blob);
  blob = NULL;
  if (!created || !CompileShader("MoveLiquidsHorizontal", "cs_5_0", &blob))
    return false;
  created = Check(ID3D11Device_CreateComputeShader(application->device, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), NULL, &application->liquid_horizontal_shader), "CreateComputeShader liquid horizontal");
  Release(blob);
  blob = NULL;
  if (!created || !CompileShader("ApplyBrush", "cs_5_0", &blob))
    return false;
  created = Check(ID3D11Device_CreateComputeShader(application->device, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), NULL, &application->brush_shader), "CreateComputeShader brush");
  Release(blob);
  blob = NULL;
  if (!created || !CompileShader("FullscreenVertex", "vs_5_0", &blob))
    return false;
  created = Check(ID3D11Device_CreateVertexShader(application->device, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), NULL, &application->vertex_shader), "CreateVertexShader");
  Release(blob);
  blob = NULL;
  if (!created || !CompileShader("DrawPixels", "ps_5_0", &blob))
    return false;
  created = Check(ID3D11Device_CreatePixelShader(application->device, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), NULL, &application->pixel_shader), "CreatePixelShader");
  Release(blob);
  blob = NULL;
  if (!created || !CompileShader("DrawBloomEmission", "ps_5_0", &blob))
    return false;
  created = Check(ID3D11Device_CreatePixelShader(
                      application->device,
                      ID3D10Blob_GetBufferPointer(blob),
                      ID3D10Blob_GetBufferSize(blob), NULL,
                      &application->bloom_emission_shader),
                  "CreatePixelShader bloom emission");
  Release(blob);
  blob = NULL;
  if (!created || !CompileShader("BlurBloomHorizontal", "ps_5_0", &blob))
    return false;
  created = Check(ID3D11Device_CreatePixelShader(
                      application->device,
                      ID3D10Blob_GetBufferPointer(blob),
                      ID3D10Blob_GetBufferSize(blob), NULL,
                      &application->bloom_blur_horizontal_shader),
                  "CreatePixelShader bloom blur horizontal");
  Release(blob);
  blob = NULL;
  if (!created || !CompileShader("BlurBloomVertical", "ps_5_0", &blob))
    return false;
  created = Check(ID3D11Device_CreatePixelShader(
                      application->device,
                      ID3D10Blob_GetBufferPointer(blob),
                      ID3D10Blob_GetBufferSize(blob), NULL,
                      &application->bloom_blur_vertical_shader),
                  "CreatePixelShader bloom blur vertical");
  Release(blob);
  blob = NULL;
  if (!created || !CompileShader("CompositeBloom", "ps_5_0", &blob))
    return false;
  created = Check(ID3D11Device_CreatePixelShader(
                      application->device,
                      ID3D10Blob_GetBufferPointer(blob),
                      ID3D10Blob_GetBufferSize(blob), NULL,
                      &application->bloom_composite_shader),
                  "CreatePixelShader bloom composite");
  Release(blob);
  if (!created)
    return false;
  if (!CreateStateTexture(application, 0) || !CreateStateTexture(application, 1))
    return false;
  const UINT empty[4] = {0, 0, 0, 0};
  ID3D11DeviceContext_ClearUnorderedAccessViewUint(application->context,
                                                   application->state_uav[0],
                                                   empty);
  ID3D11DeviceContext_ClearUnorderedAccessViewUint(application->context,
                                                   application->state_uav[1],
                                                   empty);
  return true;
}

static bool UpdateConstants(app* application) {
  D3D11_MAPPED_SUBRESOURCE mapped;
  HRESULT result = ID3D11DeviceContext_Map(
      application->context, (ID3D11Resource*)application->constants, 0,
      D3D11_MAP_WRITE_DISCARD, 0, &mapped);
  if (!Check(result, "Map frame constants"))
    return false;
  gpu_frame_constants* constants = mapped.pData;
  constants->frame_index = application->frame_index;
  constants->simulation_size[0] = SIM_WIDTH;
  constants->simulation_size[1] = SIM_HEIGHT;
  constants->selected_pixel = (uint32_t)application->selected;
  constants->viewport_origin[0] = (uint32_t)application->viewport.x;
  constants->viewport_origin[1] = (uint32_t)application->viewport.y;
  constants->viewport_size[0] = (uint32_t)application->viewport.width;
  constants->viewport_size[1] = (uint32_t)application->viewport.height;
  constants->brush_origin[0] = (uint32_t)application->brush_bounds.x;
  constants->brush_origin[1] = (uint32_t)application->brush_bounds.y;
  constants->brush_extent[0] = (uint32_t)application->brush_bounds.width;
  constants->brush_extent[1] = (uint32_t)application->brush_bounds.height;
  constants->horizontal_phase = application->horizontal_phase;
  memset(constants->frame_padding, 0, sizeof(constants->frame_padding));
  ID3D11DeviceContext_Unmap(application->context,
                            (ID3D11Resource*)application->constants, 0);
  return true;
}

static void Dispatch(app* application, ID3D11ComputeShader* shader) {
  uint32_t write_state = 1u - application->read_state;
  ID3D11ShaderResourceView* srvs[] = {application->state_srv[application->read_state], application->brush_srv};
  ID3D11UnorderedAccessView* uavs[] = {application->state_uav[write_state]};
  ID3D11Buffer* constants[] = {application->constants};
  ID3D11DeviceContext_CSSetShader(application->context, shader, NULL, 0);
  ID3D11DeviceContext_CSSetShaderResources(application->context, 0, 2, srvs);
  ID3D11DeviceContext_CSSetUnorderedAccessViews(application->context, 0, 1, uavs, NULL);
  ID3D11DeviceContext_CSSetConstantBuffers(application->context, 0, 1, constants);
  uint32_t group_width = (SIM_WIDTH + 7u) / 8u;
  uint32_t group_height = (SIM_HEIGHT + 7u) / 8u;
  if (shader == application->simulate_shader) {
    const uint32_t thread_width = (SIM_WIDTH + 2u) / 2u;
    const uint32_t thread_height = (SIM_HEIGHT + 2u) / 2u;
    group_width = (thread_width + 7u) / 8u;
    group_height = (thread_height + 7u) / 8u;
  } else if (shader == application->falling_vertical_shader) {
    group_width = (SIM_WIDTH + 63u) / 64u;
    group_height = 1u;
  } else if (shader == application->liquid_horizontal_shader) {
    group_width = (SIM_HEIGHT + 63u) / 64u;
    group_height = 1u;
  }
  ID3D11DeviceContext_Dispatch(application->context, group_width,
                               group_height, 1);
  {
    ID3D11ShaderResourceView* null_srvs[] = {NULL, NULL};
    ID3D11UnorderedAccessView* null_uavs[] = {NULL};
    ID3D11DeviceContext_CSSetShaderResources(application->context, 0, 2, null_srvs);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(application->context, 0, 1, null_uavs, NULL);
  }
  application->read_state = write_state;
}

static bool DispatchHorizontalInPlace(app* application, uint32_t phase) {
  application->horizontal_phase = phase;
  if (!UpdateConstants(application))
    return false;

  ID3D11UnorderedAccessView* state =
      application->state_uav[application->read_state];
  ID3D11Buffer* constants = application->constants;
  ID3D11DeviceContext_CSSetShader(application->context,
                                  application->liquid_horizontal_shader, NULL,
                                  0);
  ID3D11DeviceContext_CSSetUnorderedAccessViews(application->context, 0, 1,
                                                &state, NULL);
  ID3D11DeviceContext_CSSetConstantBuffers(application->context, 0, 1,
                                           &constants);
  const uint32_t row_count =
      phase < SIM_HEIGHT ? (SIM_HEIGHT - phase + 2u) / 3u : 0u;
  ID3D11DeviceContext_Dispatch(application->context, 1, row_count, 1);
  {
    ID3D11UnorderedAccessView* null_state = NULL;
    ID3D11DeviceContext_CSSetUnorderedAccessViews(application->context, 0, 1,
                                                  &null_state, NULL);
  }
  return true;
}

static bool RunSimulationUpdate(app* application) {
  ID3D11ComputeShader* ping_pong_passes[] = {
      application->simulate_shader,
      application->falling_vertical_shader,
  };
  _Static_assert(sizeof(ping_pong_passes) / sizeof(ping_pong_passes[0]) == 2u,
                 "the local and vertical passes ping-pong state");

  if (!UpdateConstants(application))
    return false;
  for (uint32_t pass = 0; pass < 2u; ++pass)
    Dispatch(application, ping_pong_passes[pass]);
  static const uint32_t phase_orders[6][3] = {
      {2u, 1u, 0u},
      {0u, 2u, 1u},
      {1u, 0u, 2u},
      {0u, 1u, 2u},
      {1u, 2u, 0u},
      {2u, 0u, 1u},
  };
  const uint32_t phase_order = application->frame_index % 6u;
  for (uint32_t pass = 0; pass < 3u; ++pass) {
    const uint32_t phase = phase_orders[phase_order][pass];
    if (!DispatchHorizontalInPlace(application, phase))
      return false;
  }
  ++application->frame_index;
  return true;
}

static bool MeasureSimulationGpuMilliseconds(app* application,
                                             uint32_t update_count,
                                             double* milliseconds_per_update) {
  D3D11_QUERY_DESC disjoint_description = {
      D3D11_QUERY_TIMESTAMP_DISJOINT,
      0,
  };
  D3D11_QUERY_DESC timestamp_description = {
      D3D11_QUERY_TIMESTAMP,
      0,
  };
  ID3D11Query* disjoint_query = NULL;
  ID3D11Query* start_query = NULL;
  ID3D11Query* end_query = NULL;
  if (FAILED(ID3D11Device_CreateQuery(application->device,
                                      &disjoint_description,
                                      &disjoint_query)) ||
      FAILED(ID3D11Device_CreateQuery(application->device,
                                      &timestamp_description, &start_query)) ||
      FAILED(ID3D11Device_CreateQuery(application->device,
                                      &timestamp_description, &end_query))) {
    Release(disjoint_query);
    Release(start_query);
    Release(end_query);
    return false;
  }

  ID3D11DeviceContext_Begin(application->context,
                            (ID3D11Asynchronous*)disjoint_query);
  ID3D11DeviceContext_End(application->context,
                          (ID3D11Asynchronous*)start_query);
  for (uint32_t update = 0; update < update_count; ++update) {
    if (!RunSimulationUpdate(application)) {
      Release(disjoint_query);
      Release(start_query);
      Release(end_query);
      return false;
    }
  }
  ID3D11DeviceContext_End(application->context,
                          (ID3D11Asynchronous*)end_query);
  ID3D11DeviceContext_End(application->context,
                          (ID3D11Asynchronous*)disjoint_query);
  ID3D11DeviceContext_Flush(application->context);

  D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint = {0};
  uint64_t start = 0;
  uint64_t end = 0;
  bool ready = false;
  for (uint32_t attempt = 0; attempt < 10000u; ++attempt) {
    const HRESULT disjoint_result = ID3D11DeviceContext_GetData(
        application->context, (ID3D11Asynchronous*)disjoint_query, &disjoint,
        sizeof(disjoint), 0);
    const HRESULT start_result = ID3D11DeviceContext_GetData(
        application->context, (ID3D11Asynchronous*)start_query, &start,
        sizeof(start), 0);
    const HRESULT end_result = ID3D11DeviceContext_GetData(
        application->context, (ID3D11Asynchronous*)end_query, &end,
        sizeof(end), 0);
    if (FAILED(disjoint_result) || FAILED(start_result) || FAILED(end_result))
      break;
    if (disjoint_result == S_OK && start_result == S_OK && end_result == S_OK) {
      ready = true;
      break;
    }
    Sleep(1);
  }

  Release(disjoint_query);
  Release(start_query);
  Release(end_query);
  if (!ready || disjoint.Disjoint || disjoint.Frequency == 0 || end < start)
    return false;
  *milliseconds_per_update =
      ((double)(end - start) * 1000.0) /
      ((double)disjoint.Frequency * (double)update_count);
  return true;
}

static bool ApplyBrush(app* application, sim_brush_command command) {
  D3D11_MAPPED_SUBRESOURCE mapped;
  HRESULT result = ID3D11DeviceContext_Map(
      application->context, (ID3D11Resource*)application->brush_buffer, 0,
      D3D11_MAP_WRITE_DISCARD, 0, &mapped);
  if (!Check(result, "Map brush command"))
    return false;
  *(sim_brush_command*)mapped.pData = command;
  ID3D11DeviceContext_Unmap(application->context,
                            (ID3D11Resource*)application->brush_buffer, 0);
  application->brush_bounds = sim_clip_brush_bounds(command);
  if (!UpdateConstants(application))
    return false;
  ID3D11ShaderResourceView* brush = application->brush_srv;
  ID3D11UnorderedAccessView* state =
      application->state_uav[application->read_state];
  ID3D11Buffer* constants = application->constants;
  ID3D11DeviceContext_CSSetShader(application->context,
                                  application->brush_shader, NULL, 0);
  ID3D11DeviceContext_CSSetShaderResources(application->context, 1, 1, &brush);
  ID3D11DeviceContext_CSSetUnorderedAccessViews(application->context, 0, 1,
                                                &state, NULL);
  ID3D11DeviceContext_CSSetConstantBuffers(application->context, 0, 1,
                                           &constants);
  ID3D11DeviceContext_Dispatch(
      application->context,
      ((uint32_t)application->brush_bounds.width + 7u) / 8u,
      ((uint32_t)application->brush_bounds.height + 7u) / 8u, 1);
  {
    ID3D11ShaderResourceView* null_brush = NULL;
    ID3D11UnorderedAccessView* null_state = NULL;
    ID3D11DeviceContext_CSSetShaderResources(application->context, 1, 1,
                                             &null_brush);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(application->context, 0, 1,
                                                  &null_state, NULL);
  }
  return true;
}

static float TextWidth(const app* application, const char* text) {
  float width = 0.0f;
  for (const unsigned char* character = (const unsigned char*)text; *character;
       ++character) {
    if (*character >= FONT_FIRST_CHARACTER &&
        *character < FONT_FIRST_CHARACTER + FONT_CHARACTER_COUNT)
      width += application->text
                   .characters[*character - FONT_FIRST_CHARACTER]
                   .xadvance;
  }
  return width;
}

static void SetTextVertex(text_vertex* vertex,
                          float x,
                          float y,
                          float u,
                          float v,
                          const float color[4],
                          int client_width,
                          int client_height) {
  vertex->position[0] = x * 2.0f / (float)client_width - 1.0f;
  vertex->position[1] = 1.0f - y * 2.0f / (float)client_height;
  vertex->uv[0] = u;
  vertex->uv[1] = v;
  memcpy(vertex->color, color, sizeof(vertex->color));
}

static void AddTextQuad(app* application,
                        text_vertex* vertices,
                        uint32_t* vertex_count,
                        float x0,
                        float y0,
                        float x1,
                        float y1,
                        float u0,
                        float v0,
                        float u1,
                        float v1,
                        const float color[4]) {
  if (*vertex_count + 6u > TEXT_MAX_VERTICES)
    return;
  text_vertex* quad = vertices + *vertex_count;
  SetTextVertex(&quad[0], x0, y0, u0, v0, color, application->client_width,
                application->client_height);
  SetTextVertex(&quad[1], x1, y0, u1, v0, color, application->client_width,
                application->client_height);
  SetTextVertex(&quad[2], x1, y1, u1, v1, color, application->client_width,
                application->client_height);
  SetTextVertex(&quad[3], x0, y0, u0, v0, color, application->client_width,
                application->client_height);
  SetTextVertex(&quad[4], x1, y1, u1, v1, color, application->client_width,
                application->client_height);
  SetTextVertex(&quad[5], x0, y1, u0, v1, color, application->client_width,
                application->client_height);
  *vertex_count += 6u;
}

static void AddText(app* application,
                    text_vertex* vertices,
                    uint32_t* vertex_count,
                    const char* text,
                    float x,
                    float baseline,
                    const float color[4]) {
  float pen_x = x;
  float pen_y = baseline;
  for (const unsigned char* character = (const unsigned char*)text; *character;
       ++character) {
    if (*character < FONT_FIRST_CHARACTER ||
        *character >= FONT_FIRST_CHARACTER + FONT_CHARACTER_COUNT)
      continue;
    stbtt_aligned_quad quad;
    stbtt_GetBakedQuad(application->text.characters, FONT_ATLAS_SIZE,
                       FONT_ATLAS_SIZE, *character - FONT_FIRST_CHARACTER,
                       &pen_x, &pen_y, &quad, 1);
    AddTextQuad(application, vertices, vertex_count, quad.x0, quad.y0, quad.x1,
                quad.y1, quad.s0, quad.t0, quad.s1, quad.t1, color);
  }
}

static char MaterialShortcut(sim_pixel_type material) {
  if (material == SIM_PIXEL_EMPTY)
    return '0';
  if (material >= SIM_PIXEL_WOOD && material <= SIM_PIXEL_ACID)
    return (char)('0' + material);
  if (material >= SIM_PIXEL_FIRE && material <= SIM_PIXEL_BEDROCK)
    return (char)('A' + material - SIM_PIXEL_FIRE);
  return '?';
}

static const char* MaterialUiName(sim_pixel_type material) {
  return material == SIM_PIXEL_EMPTY ? "Erase" : sim_material_name(material);
}

static bool RenderTooltip(app* application) {
  static const char* descriptions[SIM_PIXEL_TYPE_COUNT] = {
      "Remove material from the brush area.",
      "Rigid fuel that catches fire near heat.",
      "Heavy solid that rust and acid can erode.",
      "Granular powder that falls and piles up.",
      "Granular corrosion that spreads through iron.",
      "Dense liquid that cools lava and extinguishes fire.",
      "Hot liquid that ignites fuel and turns water to steam.",
      "Light gas that rises and drifts.",
      "Hot vapor that rises and can condense into water.",
      "Corrosive liquid that dissolves most materials.",
      "Hot gas that spreads through wood and oil.",
      "Buoyant liquid fuel that floats on water.",
      "Granular mineral that slowly dissolves in water.",
      "Permanent solid that resists reactions.",
  };
  if (application->hovered >= SIM_PIXEL_TYPE_COUNT ||
      application->client_width <= 0 || application->client_height <= 0)
    return true;

  char title[64];
  snprintf(title, sizeof(title), "%s  [%c]",
           MaterialUiName(application->hovered),
           MaterialShortcut(application->hovered));
  const char* description = descriptions[application->hovered];
  float tooltip_width = TextWidth(application, title);
  const float description_width = TextWidth(application, description);
  if (description_width > tooltip_width)
    tooltip_width = description_width;
  tooltip_width += 20.0f;
  const float tooltip_height = 54.0f;
  float tooltip_x = (float)application->mouse_x + 14.0f;
  float tooltip_y = (float)application->mouse_y + 18.0f;
  if (tooltip_x + tooltip_width > application->client_width - 6.0f)
    tooltip_x = (float)application->client_width - tooltip_width - 6.0f;
  if (tooltip_y + tooltip_height > application->client_height - 6.0f)
    tooltip_y = (float)application->mouse_y - tooltip_height - 10.0f;
  if (tooltip_x < 6.0f)
    tooltip_x = 6.0f;
  if (tooltip_y < 6.0f)
    tooltip_y = 6.0f;

  text_vertex cpu_vertices[TEXT_MAX_VERTICES];
  uint32_t vertex_count = 0;
  const float white_pixel = 0.5f / FONT_ATLAS_SIZE;
  const float shadow[4] = {0.0f, 0.0f, 0.0f, 0.35f};
  const float background[4] = {0.055f, 0.065f, 0.08f, 0.96f};
  const float border[4] = {0.42f, 0.46f, 0.52f, 1.0f};
  const float title_color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  const float description_color[4] = {0.82f, 0.85f, 0.90f, 1.0f};
  AddTextQuad(application, cpu_vertices, &vertex_count, tooltip_x + 3.0f,
              tooltip_y + 3.0f, tooltip_x + tooltip_width + 3.0f,
              tooltip_y + tooltip_height + 3.0f, white_pixel, white_pixel,
              white_pixel, white_pixel, shadow);
  AddTextQuad(application, cpu_vertices, &vertex_count, tooltip_x, tooltip_y,
              tooltip_x + tooltip_width, tooltip_y + tooltip_height,
              white_pixel, white_pixel, white_pixel, white_pixel, border);
  AddTextQuad(application, cpu_vertices, &vertex_count, tooltip_x + 1.0f,
              tooltip_y + 1.0f, tooltip_x + tooltip_width - 1.0f,
              tooltip_y + tooltip_height - 1.0f, white_pixel, white_pixel,
              white_pixel, white_pixel, background);
  AddText(application, cpu_vertices, &vertex_count, title, tooltip_x + 10.0f,
          tooltip_y + 21.0f, title_color);
  AddText(application, cpu_vertices, &vertex_count, description,
          tooltip_x + 10.0f, tooltip_y + 43.0f, description_color);

  D3D11_MAPPED_SUBRESOURCE mapped;
  if (!Check(ID3D11DeviceContext_Map(
                 application->context,
                 (ID3D11Resource*)application->text.vertices, 0,
                 D3D11_MAP_WRITE_DISCARD, 0, &mapped),
             "Map text vertices"))
    return false;
  memcpy(mapped.pData, cpu_vertices, vertex_count * sizeof(text_vertex));
  ID3D11DeviceContext_Unmap(application->context,
                            (ID3D11Resource*)application->text.vertices, 0);

  D3D11_VIEWPORT viewport = {0};
  viewport.Width = (FLOAT)application->client_width;
  viewport.Height = (FLOAT)application->client_height;
  viewport.MaxDepth = 1.0f;
  ID3D11DeviceContext_RSSetViewports(application->context, 1, &viewport);
  const UINT stride = sizeof(text_vertex);
  const UINT offset = 0;
  ID3D11DeviceContext_IASetInputLayout(application->context,
                                       application->text.input_layout);
  ID3D11DeviceContext_IASetVertexBuffers(application->context, 0, 1,
                                         &application->text.vertices, &stride,
                                         &offset);
  ID3D11DeviceContext_IASetPrimitiveTopology(
      application->context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ID3D11DeviceContext_VSSetShader(application->context,
                                  application->text.vertex_shader, NULL, 0);
  ID3D11DeviceContext_PSSetShader(application->context,
                                  application->text.pixel_shader, NULL, 0);
  ID3D11DeviceContext_PSSetShaderResources(
      application->context, 0, 1, &application->text.atlas_srv);
  ID3D11DeviceContext_PSSetSamplers(application->context, 0, 1,
                                    &application->text.sampler);
  const FLOAT blend_factor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  ID3D11DeviceContext_OMSetBlendState(application->context,
                                      application->text.blend, blend_factor,
                                      0xffffffffu);
  ID3D11DeviceContext_Draw(application->context, vertex_count, 0);
  {
    ID3D11ShaderResourceView* null_atlas = NULL;
    ID3D11DeviceContext_PSSetShaderResources(application->context, 0, 1,
                                             &null_atlas);
  }
  ID3D11DeviceContext_OMSetBlendState(application->context, NULL, NULL,
                                      0xffffffffu);
  return true;
}

static void UpdateFps(app* application);

static void DrawPostProcessPass(app* application,
                                ID3D11RenderTargetView* target,
                                ID3D11PixelShader* shader,
                                ID3D11ShaderResourceView* source,
                                uint32_t width,
                                uint32_t height) {
  const FLOAT clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  ID3D11DeviceContext_OMSetRenderTargets(application->context, 1, &target,
                                         NULL);
  ID3D11DeviceContext_ClearRenderTargetView(application->context, target,
                                            clear);
  D3D11_VIEWPORT viewport = {0};
  viewport.Width = (FLOAT)width;
  viewport.Height = (FLOAT)height;
  viewport.MaxDepth = 1.0f;
  ID3D11DeviceContext_RSSetViewports(application->context, 1, &viewport);
  ID3D11DeviceContext_IASetInputLayout(application->context, NULL);
  ID3D11DeviceContext_IASetPrimitiveTopology(
      application->context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ID3D11DeviceContext_VSSetShader(application->context,
                                  application->vertex_shader, NULL, 0);
  ID3D11DeviceContext_PSSetShader(application->context, shader, NULL, 0);
  ID3D11DeviceContext_PSSetShaderResources(application->context, 2, 1,
                                           &source);
  ID3D11DeviceContext_PSSetSamplers(application->context, 1, 1,
                                    &application->linear_sampler);
  ID3D11DeviceContext_Draw(application->context, 3, 0);
  {
    ID3D11ShaderResourceView* null_source = NULL;
    ID3D11DeviceContext_PSSetShaderResources(application->context, 2, 1,
                                             &null_source);
  }
}

static void DrawBloomEmission(app* application,
                              uint32_t bloom_width,
                              uint32_t bloom_height) {
  const FLOAT clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  ID3D11DeviceContext_OMSetRenderTargets(
      application->context, 1, &application->bloom_target[0], NULL);
  ID3D11DeviceContext_ClearRenderTargetView(
      application->context, application->bloom_target[0], clear);
  D3D11_VIEWPORT viewport = {0};
  viewport.TopLeftX = (FLOAT)application->viewport.x * bloom_width /
                      application->client_width;
  viewport.TopLeftY = (FLOAT)application->viewport.y * bloom_height /
                      application->client_height;
  viewport.Width = (FLOAT)application->viewport.width * bloom_width /
                   application->client_width;
  viewport.Height = (FLOAT)application->viewport.height * bloom_height /
                    application->client_height;
  viewport.MaxDepth = 1.0f;
  ID3D11DeviceContext_RSSetViewports(application->context, 1, &viewport);
  ID3D11DeviceContext_IASetInputLayout(application->context, NULL);
  ID3D11DeviceContext_IASetPrimitiveTopology(
      application->context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ID3D11DeviceContext_VSSetShader(application->context,
                                  application->vertex_shader, NULL, 0);
  ID3D11DeviceContext_PSSetShader(application->context,
                                  application->bloom_emission_shader, NULL,
                                  0);
  ID3D11ShaderResourceView* state =
      application->state_srv[application->read_state];
  ID3D11Buffer* constants = application->constants;
  ID3D11DeviceContext_PSSetShaderResources(application->context, 0, 1,
                                           &state);
  ID3D11DeviceContext_PSSetSamplers(application->context, 0, 1,
                                    &application->point_sampler);
  ID3D11DeviceContext_PSSetConstantBuffers(application->context, 0, 1,
                                           &constants);
  ID3D11DeviceContext_Draw(application->context, 3, 0);
  {
    ID3D11ShaderResourceView* null_state = NULL;
    ID3D11DeviceContext_PSSetShaderResources(application->context, 0, 1,
                                             &null_state);
  }
}

static void RenderBloom(app* application) {
  const uint32_t bloom_width =
      (uint32_t)(application->client_width > 1
                     ? application->client_width / 2
                     : 1);
  const uint32_t bloom_height =
      (uint32_t)(application->client_height > 1
                     ? application->client_height / 2
                     : 1);
  DrawBloomEmission(application, bloom_width, bloom_height);
  DrawPostProcessPass(application, application->bloom_target[1],
                      application->bloom_blur_horizontal_shader,
                      application->bloom_srv[0], bloom_width, bloom_height);
  DrawPostProcessPass(application, application->bloom_target[0],
                      application->bloom_blur_vertical_shader,
                      application->bloom_srv[1], bloom_width, bloom_height);

  ID3D11DeviceContext_OMSetRenderTargets(application->context, 1,
                                         &application->backbuffer, NULL);
  D3D11_VIEWPORT viewport = {0};
  viewport.Width = (FLOAT)application->client_width;
  viewport.Height = (FLOAT)application->client_height;
  viewport.MaxDepth = 1.0f;
  ID3D11DeviceContext_RSSetViewports(application->context, 1, &viewport);
  ID3D11ShaderResourceView* sources[] = {
      application->scene_srv,
      application->bloom_srv[0],
  };
  ID3D11DeviceContext_PSSetShader(application->context,
                                  application->bloom_composite_shader, NULL,
                                  0);
  ID3D11DeviceContext_PSSetShaderResources(application->context, 2, 2,
                                           sources);
  ID3D11DeviceContext_Draw(application->context, 3, 0);
  {
    ID3D11ShaderResourceView* null_sources[] = {NULL, NULL};
    ID3D11DeviceContext_PSSetShaderResources(application->context, 2, 2,
                                             null_sources);
  }
}

static bool Render(app* application) {
  if (application->viewport.width <= 0 || application->viewport.height <= 0) {
    Sleep(10);
    return true;
  }
  if (!UpdateConstants(application))
    return false;
  sim_brush_command command;
  while (sim_brush_queue_pop(&application->brush_queue, &command)) {
    if (!ApplyBrush(application, command))
      return false;
  }
  LARGE_INTEGER simulation_now;
  QueryPerformanceCounter(&simulation_now);
  uint64_t elapsed_ticks =
      simulation_now.QuadPart > application->simulation_last.QuadPart
          ? (uint64_t)(simulation_now.QuadPart -
                       application->simulation_last.QuadPart)
          : 0;
  application->simulation_last = simulation_now;
  const uint32_t simulation_steps = sim_consume_fixed_steps(
      &application->simulation_accumulator, elapsed_ticks,
      (uint64_t)application->fps_frequency.QuadPart);
  for (uint32_t step = 0; step < simulation_steps; ++step) {
    if (!RunSimulationUpdate(application))
      return false;
  }
  FLOAT clear[] = {0.02f, 0.03f, 0.04f, 1.0f};
  ID3D11DeviceContext_OMSetRenderTargets(application->context, 1,
                                         &application->scene_target, NULL);
  ID3D11DeviceContext_ClearRenderTargetView(application->context,
                                            application->scene_target, clear);
  D3D11_VIEWPORT simulation_viewport = {0};
  simulation_viewport.TopLeftX = (FLOAT)application->viewport.x;
  simulation_viewport.TopLeftY = (FLOAT)application->viewport.y;
  simulation_viewport.Width = (FLOAT)application->viewport.width;
  simulation_viewport.Height = (FLOAT)application->viewport.height;
  simulation_viewport.MaxDepth = 1.0f;
  ID3D11DeviceContext_RSSetViewports(application->context, 1,
                                     &simulation_viewport);
  ID3D11ShaderResourceView* state = application->state_srv[application->read_state];
  ID3D11Buffer* constants = application->constants;
  ID3D11DeviceContext_VSSetShader(application->context, application->vertex_shader, NULL, 0);
  ID3D11DeviceContext_PSSetShader(application->context, application->pixel_shader, NULL, 0);
  ID3D11DeviceContext_PSSetShaderResources(application->context, 0, 1, &state);
  ID3D11DeviceContext_PSSetSamplers(application->context, 0, 1, &application->point_sampler);
  ID3D11DeviceContext_PSSetConstantBuffers(application->context, 0, 1, &constants);
  ID3D11DeviceContext_IASetInputLayout(application->context, NULL);
  ID3D11DeviceContext_IASetPrimitiveTopology(application->context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ID3D11DeviceContext_Draw(application->context, 3, 0);
  {
    ID3D11ShaderResourceView* null_state = NULL;
    ID3D11DeviceContext_PSSetShaderResources(application->context, 0, 1, &null_state);
  }
  RenderBloom(application);
  if (!RenderTooltip(application))
    return false;
  if (!Check(IDXGISwapChain_Present(application->swap_chain, 1, 0), "Present"))
    return false;
  UpdateFps(application);
  return true;
}

static void UpdateWindowTitle(app* application) {
  char title[128];
  snprintf(title, sizeof(title),
           "PixelSim | %s | FPS: %u | Palette keys: 0-9, A-D",
           MaterialUiName(application->selected), application->fps);
  SetWindowTextA(application->window, title);
}

static void UpdateFps(app* application) {
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  ++application->fps_frames;
  if (now.QuadPart - application->fps_start.QuadPart < application->fps_frequency.QuadPart)
    return;
  application->fps = (uint32_t)((application->fps_frames * application->fps_frequency.QuadPart) /
                                (now.QuadPart - application->fps_start.QuadPart));
  application->fps_frames = 0;
  application->fps_start = now;
  UpdateWindowTitle(application);
}

static bool SelectMaterialAtPoint(app* application, int x, int y) {
  sim_pixel_type material =
      sim_palette_material_at(application->viewport, x, y);
  if (material >= SIM_PIXEL_TYPE_COUNT)
    return false;
  application->selected = material;
  UpdateWindowTitle(application);
  return true;
}

static bool QueueBrushAtPoint(app* application, int x, int y) {
  if (sim_palette_material_at(application->viewport, x, y) !=
      SIM_PIXEL_TYPE_COUNT)
    return false;
  int cell_x;
  int cell_y;
  if (!sim_viewport_to_cell(application->viewport, x, y, &cell_x, &cell_y))
    return false;
  sim_brush_command command = sim_make_brush_command(
      application->selected, cell_x, cell_y, 5,
      application->frame_index + application->brush_queue.count);
  return sim_brush_queue_push(&application->brush_queue, command);
}

static bool ResizeSwapChain(app* application, int width, int height) {
  application->client_width = width;
  application->client_height = height;
  application->viewport = sim_make_viewport(width, height);
  if (width <= 0 || height <= 0)
    return true;

  ID3D11DeviceContext_OMSetRenderTargets(application->context, 0, NULL, NULL);
  ReleasePostProcessTargets(application);
  Release(application->backbuffer);
  application->backbuffer = NULL;
  if (!Check(IDXGISwapChain_ResizeBuffers(application->swap_chain, 0,
                                          (UINT)width, (UINT)height,
                                          DXGI_FORMAT_UNKNOWN, 0),
             "ResizeBuffers"))
    return false;

  ID3D11Texture2D* backbuffer = NULL;
  if (!Check(IDXGISwapChain_GetBuffer(application->swap_chain, 0,
                                      &IID_ID3D11Texture2D,
                                      (void**)&backbuffer),
             "GetBuffer") ||
      !Check(ID3D11Device_CreateRenderTargetView(
                 application->device, (ID3D11Resource*)backbuffer, NULL,
                 &application->backbuffer),
             "CreateRenderTargetView")) {
    Release(backbuffer);
    return false;
  }
  Release(backbuffer);
  if (!CreatePostProcessTargets(application, width, height))
    return false;

  D3D11_VIEWPORT viewport = {0};
  viewport.TopLeftX = (FLOAT)application->viewport.x;
  viewport.TopLeftY = (FLOAT)application->viewport.y;
  viewport.Width = (FLOAT)application->viewport.width;
  viewport.Height = (FLOAT)application->viewport.height;
  viewport.MinDepth = 0.0f;
  viewport.MaxDepth = 1.0f;
  ID3D11DeviceContext_RSSetViewports(application->context, 1, &viewport);
  return true;
}

static uint32_t CountOccupiedPixels(app* application) {
  D3D11_TEXTURE2D_DESC description;
  ID3D11Texture2D_GetDesc(application->state[application->read_state],
                          &description);
  description.Usage = D3D11_USAGE_STAGING;
  description.BindFlags = 0;
  description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  description.MiscFlags = 0;

  ID3D11Texture2D* staging = NULL;
  if (!Check(ID3D11Device_CreateTexture2D(application->device, &description,
                                          NULL, &staging),
             "CreateTexture2D verification"))
    return UINT32_MAX;
  ID3D11DeviceContext_CopyResource(
      application->context, (ID3D11Resource*)staging,
      (ID3D11Resource*)application->state[application->read_state]);

  D3D11_MAPPED_SUBRESOURCE mapped;
  if (!Check(ID3D11DeviceContext_Map(application->context,
                                     (ID3D11Resource*)staging, 0,
                                     D3D11_MAP_READ, 0, &mapped),
             "Map verification")) {
    Release(staging);
    return UINT32_MAX;
  }
  uint32_t count = 0;
  for (uint32_t y = 0; y < SIM_HEIGHT; ++y) {
    const uint32_t* row =
        (const uint32_t*)((const uint8_t*)mapped.pData + y * mapped.RowPitch);
    for (uint32_t x = 0; x < SIM_WIDTH; ++x) {
      if ((row[x] & SIM_PIXEL_TYPE_MASK) != SIM_PIXEL_EMPTY)
        ++count;
    }
  }
  ID3D11DeviceContext_Unmap(application->context,
                            (ID3D11Resource*)staging, 0);
  Release(staging);
  return count;
}

static uint32_t ReadStateValue(app* application, uint32_t x, uint32_t y) {
  D3D11_TEXTURE2D_DESC description;
  ID3D11Texture2D_GetDesc(application->state[application->read_state],
                          &description);
  description.Usage = D3D11_USAGE_STAGING;
  description.BindFlags = 0;
  description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  description.MiscFlags = 0;

  ID3D11Texture2D* staging = NULL;
  if (!Check(ID3D11Device_CreateTexture2D(application->device, &description,
                                          NULL, &staging),
             "CreateTexture2D pixel verification"))
    return UINT32_MAX;
  ID3D11DeviceContext_CopyResource(
      application->context, (ID3D11Resource*)staging,
      (ID3D11Resource*)application->state[application->read_state]);

  D3D11_MAPPED_SUBRESOURCE mapped;
  if (!Check(ID3D11DeviceContext_Map(application->context,
                                     (ID3D11Resource*)staging, 0,
                                     D3D11_MAP_READ, 0, &mapped),
             "Map pixel verification")) {
    Release(staging);
    return UINT32_MAX;
  }
  const uint32_t* row =
      (const uint32_t*)((const uint8_t*)mapped.pData + y * mapped.RowPitch);
  const uint32_t pixel = row[x];
  ID3D11DeviceContext_Unmap(application->context,
                            (ID3D11Resource*)staging, 0);
  Release(staging);
  return pixel;
}

static uint32_t ReadStatePixel(app* application, uint32_t x, uint32_t y) {
  return ReadStateValue(application, x, y) & SIM_PIXEL_TYPE_MASK;
}

static bool FindSinglePixelInRow(app* application,
                                 uint32_t type,
                                 uint32_t y,
                                 uint32_t first_x,
                                 uint32_t last_x,
                                 uint32_t* found_x) {
  D3D11_TEXTURE2D_DESC description;
  ID3D11Texture2D_GetDesc(application->state[application->read_state],
                          &description);
  description.Usage = D3D11_USAGE_STAGING;
  description.BindFlags = 0;
  description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  description.MiscFlags = 0;

  ID3D11Texture2D* staging = NULL;
  if (!Check(ID3D11Device_CreateTexture2D(application->device, &description,
                                          NULL, &staging),
             "CreateTexture2D row verification"))
    return false;
  ID3D11DeviceContext_CopyResource(
      application->context, (ID3D11Resource*)staging,
      (ID3D11Resource*)application->state[application->read_state]);

  D3D11_MAPPED_SUBRESOURCE mapped;
  if (!Check(ID3D11DeviceContext_Map(application->context,
                                     (ID3D11Resource*)staging, 0,
                                     D3D11_MAP_READ, 0, &mapped),
             "Map row verification")) {
    Release(staging);
    return false;
  }
  const uint32_t* row =
      (const uint32_t*)((const uint8_t*)mapped.pData + y * mapped.RowPitch);
  uint32_t matches = 0;
  for (uint32_t x = first_x; x <= last_x; ++x) {
    if ((row[x] & SIM_PIXEL_TYPE_MASK) == type) {
      *found_x = x;
      ++matches;
    }
  }
  ID3D11DeviceContext_Unmap(application->context,
                            (ID3D11Resource*)staging, 0);
  Release(staging);
  return matches == 1;
}

static bool MeasureMaterial(app* application,
                            uint32_t type,
                            uint32_t* count,
                            uint32_t* min_x,
                            uint32_t* max_x,
                            uint32_t* min_y,
                            uint32_t* max_y,
                            uint64_t* material_checksum) {
  D3D11_TEXTURE2D_DESC description;
  ID3D11Texture2D_GetDesc(application->state[application->read_state],
                          &description);
  description.Usage = D3D11_USAGE_STAGING;
  description.BindFlags = 0;
  description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  description.MiscFlags = 0;

  ID3D11Texture2D* staging = NULL;
  if (!Check(ID3D11Device_CreateTexture2D(application->device, &description,
                                          NULL, &staging),
             "CreateTexture2D material measurement"))
    return false;
  ID3D11DeviceContext_CopyResource(
      application->context, (ID3D11Resource*)staging,
      (ID3D11Resource*)application->state[application->read_state]);

  D3D11_MAPPED_SUBRESOURCE mapped;
  if (!Check(ID3D11DeviceContext_Map(application->context,
                                     (ID3D11Resource*)staging, 0,
                                     D3D11_MAP_READ, 0, &mapped),
             "Map material measurement")) {
    Release(staging);
    return false;
  }

  *count = 0;
  *min_x = SIM_WIDTH;
  *max_x = 0;
  *min_y = SIM_HEIGHT;
  *max_y = 0;
  *material_checksum = UINT64_C(1469598103934665603);
  for (uint32_t y = 0; y < SIM_HEIGHT; ++y) {
    const uint32_t* row =
        (const uint32_t*)((const uint8_t*)mapped.pData + y * mapped.RowPitch);
    for (uint32_t x = 0; x < SIM_WIDTH; ++x) {
      const uint32_t pixel_type = row[x] & SIM_PIXEL_TYPE_MASK;
      *material_checksum ^= pixel_type;
      *material_checksum *= UINT64_C(1099511628211);
      if (pixel_type != type)
        continue;
      ++*count;
      *min_x = x < *min_x ? x : *min_x;
      *max_x = x > *max_x ? x : *max_x;
      *min_y = y < *min_y ? y : *min_y;
      *max_y = y > *max_y ? y : *max_y;
    }
  }

  ID3D11DeviceContext_Unmap(application->context,
                            (ID3D11Resource*)staging, 0);
  Release(staging);
  return true;
}

static bool MeasureBasinSurface(app* application,
                                uint32_t type,
                                uint32_t first_x,
                                uint32_t last_x,
                                uint32_t floor_y,
                                uint32_t* shallowest_depth,
                                uint32_t* deepest_depth,
                                uint32_t* holes) {
  D3D11_TEXTURE2D_DESC description;
  ID3D11Texture2D_GetDesc(application->state[application->read_state],
                          &description);
  description.Usage = D3D11_USAGE_STAGING;
  description.BindFlags = 0;
  description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  description.MiscFlags = 0;

  ID3D11Texture2D* staging = NULL;
  if (!Check(ID3D11Device_CreateTexture2D(application->device, &description,
                                          NULL, &staging),
             "CreateTexture2D basin measurement"))
    return false;
  ID3D11DeviceContext_CopyResource(
      application->context, (ID3D11Resource*)staging,
      (ID3D11Resource*)application->state[application->read_state]);

  D3D11_MAPPED_SUBRESOURCE mapped;
  if (!Check(ID3D11DeviceContext_Map(application->context,
                                     (ID3D11Resource*)staging, 0,
                                     D3D11_MAP_READ, 0, &mapped),
             "Map basin measurement")) {
    Release(staging);
    return false;
  }

  *shallowest_depth = floor_y;
  *deepest_depth = 0;
  *holes = 0;
  for (uint32_t x = first_x; x <= last_x; ++x) {
    uint32_t depth = 0;
    bool found_empty_below_liquid = false;
    for (uint32_t y = floor_y; y-- > 0;) {
      const uint32_t* row = (const uint32_t*)((const uint8_t*)mapped.pData +
                                              y * mapped.RowPitch);
      if ((row[x] & SIM_PIXEL_TYPE_MASK) == type) {
        ++depth;
        if (found_empty_below_liquid)
          ++*holes;
      } else if (depth != 0) {
        found_empty_below_liquid = true;
      } else {
        break;
      }
    }
    *shallowest_depth = depth < *shallowest_depth ? depth : *shallowest_depth;
    *deepest_depth = depth > *deepest_depth ? depth : *deepest_depth;
  }

  ID3D11DeviceContext_Unmap(application->context,
                            (ID3D11Resource*)staging, 0);
  Release(staging);
  return true;
}

static void ClearSimulation(app* application) {
  const UINT empty[4] = {0, 0, 0, 0};
  ID3D11DeviceContext_ClearUnorderedAccessViewUint(
      application->context, application->state_uav[0], empty);
  ID3D11DeviceContext_ClearUnorderedAccessViewUint(
      application->context, application->state_uav[1], empty);
  application->read_state = 0;
  application->frame_index = 0;
}

static bool ApplyTestPixel(app* application,
                           sim_pixel_type type,
                           int x,
                           int y,
                           uint32_t seed) {
  return ApplyBrush(
      application,
      (sim_brush_command){(uint32_t)type, x, y, 0, seed});
}

static LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  app* application = (app*)GetWindowLongPtr(window, GWLP_USERDATA);
  switch (message) {
    case WM_LBUTTONDOWN:
      if (!application)
        return 0;
      if (SelectMaterialAtPoint(application, (int)(short)LOWORD(lparam),
                                (int)(short)HIWORD(lparam)))
        return 0;
      if (QueueBrushAtPoint(application, (int)(short)LOWORD(lparam),
                            (int)(short)HIWORD(lparam))) {
        application->painting = true;
        SetCapture(window);
      }
      return 0;
    case WM_MOUSEMOVE:
      if (application) {
        application->mouse_x = (int)(short)LOWORD(lparam);
        application->mouse_y = (int)(short)HIWORD(lparam);
        application->hovered = sim_palette_material_at(
            application->viewport, application->mouse_x,
            application->mouse_y);
        if (!application->tracking_mouse) {
          TRACKMOUSEEVENT tracking = {
              sizeof(tracking), TME_LEAVE, window, HOVER_DEFAULT};
          TrackMouseEvent(&tracking);
          application->tracking_mouse = true;
        }
        if (application->painting && (wparam & MK_LBUTTON))
          QueueBrushAtPoint(application, application->mouse_x,
                            application->mouse_y);
      }
      return 0;
    case WM_MOUSELEAVE:
      if (application) {
        application->hovered = SIM_PIXEL_TYPE_COUNT;
        application->tracking_mouse = false;
      }
      return 0;
    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED:
    case WM_CANCELMODE:
    case WM_KILLFOCUS:
      if (application)
        application->painting = false;
      if (message != WM_CAPTURECHANGED && GetCapture() == window)
        ReleaseCapture();
      return 0;
    case WM_KEYDOWN:
      if (application && wparam == '0') {
        application->selected = SIM_PIXEL_EMPTY;
        UpdateWindowTitle(application);
      } else if (application && wparam >= '1' && wparam <= '9') {
        application->selected = (sim_pixel_type)(wparam - '0');
        UpdateWindowTitle(application);
      } else if (application && wparam >= 'A' && wparam <= 'D') {
        application->selected = (sim_pixel_type)(SIM_PIXEL_FIRE + wparam - 'A');
        UpdateWindowTitle(application);
      }
      return 0;
    case WM_SIZE:
      if (application && application->swap_chain &&
          !ResizeSwapChain(application, LOWORD(lparam), HIWORD(lparam)))
        PostQuitMessage(1);
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProc(window, message, wparam, lparam);
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR command_line, int show) {
  (void)previous;
  (void)show;
  bool smoke = strstr(command_line, "--smoke") != NULL;
  app application = {0};
  int exit_code = 1;
  application.selected = SIM_PIXEL_SAND;
  application.hovered = SIM_PIXEL_TYPE_COUNT;
  QueryPerformanceFrequency(&application.fps_frequency);
  QueryPerformanceCounter(&application.fps_start);
  application.simulation_last = application.fps_start;
  WNDCLASSA window_class = {0};
  window_class.hInstance = instance;
  window_class.lpszClassName = "PixelSimWindow";
  window_class.lpfnWndProc = WindowProcedure;
  window_class.hCursor = LoadCursor(NULL, IDC_CROSS);
  RegisterClassA(&window_class);
  application.window = CreateWindowExA(0, window_class.lpszClassName, "PixelSim", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 1280, 760, NULL, NULL, instance, NULL);
  if (!application.window)
    return 1;
  SetWindowLongPtr(application.window, GWLP_USERDATA, (LONG_PTR)&application);
  DXGI_SWAP_CHAIN_DESC swap = {0};
  swap.BufferCount = 2;
  swap.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  swap.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  swap.OutputWindow = application.window;
  swap.SampleDesc.Count = 1;
  swap.Windowed = TRUE;
  if (!Check(D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &swap, &application.swap_chain, &application.device, NULL, &application.context), "D3D11CreateDeviceAndSwapChain"))
    goto done;
  RECT client;
  GetClientRect(application.window, &client);
  if (!ResizeSwapChain(&application, client.right, client.bottom))
    goto done;
  if (!CreateGpuResources(&application))
    goto done;
  if (!CreateTextResources(&application))
    goto done;
  UpdateWindowTitle(&application);
  exit_code = 0;
  if (smoke) {
    uint32_t brush_cells = sim_brush_cell_count(5);
    if (!UpdateConstants(&application) ||
        !ApplyBrush(&application,
                    sim_make_brush_command(SIM_PIXEL_SAND, SIM_WIDTH / 2,
                                           SIM_HEIGHT / 3, 5, 1u))) {
      exit_code = 6;
      goto done;
    }
    if (CountOccupiedPixels(&application) != brush_cells) {
      exit_code = 3;
      goto done;
    }
    if (!ApplyBrush(&application,
                    sim_make_brush_command(SIM_PIXEL_EMPTY, SIM_WIDTH / 2,
                                           SIM_HEIGHT / 3, 5, 2u)) ||
        CountOccupiedPixels(&application) != 0u ||
        !ApplyBrush(&application,
                    sim_make_brush_command(SIM_PIXEL_SAND, SIM_WIDTH / 2,
                                           SIM_HEIGHT / 3, 5, 3u)) ||
        CountOccupiedPixels(&application) != brush_cells) {
      fprintf(stderr, "erase brush verification failed\n");
      exit_code = 35;
      goto done;
    }
    if (!ApplyBrush(&application,
                    sim_make_brush_command(SIM_PIXEL_SAND,
                                           SIM_WIDTH / 2 + 100,
                                           SIM_HEIGHT / 3, 5, 4u))) {
      exit_code = 6;
      goto done;
    }
    if (CountOccupiedPixels(&application) != brush_cells * 2u) {
      exit_code = 4;
      goto done;
    }
    for (uint32_t frame = 0; frame < 120; ++frame) {
      if (!RunSimulationUpdate(&application)) {
        exit_code = 6;
        goto done;
      }
      const uint32_t frame_count = CountOccupiedPixels(&application);
      if (frame_count != brush_cells * 2u) {
        fprintf(stderr,
                "sand conservation first failed at update %u: expected %u, got %u\n",
                frame + 1u, brush_cells * 2u, frame_count);
        exit_code = 5;
        goto done;
      }
    }
    const uint32_t conserved_sand_count = CountOccupiedPixels(&application);
    if (conserved_sand_count != brush_cells * 2u) {
      fprintf(stderr, "sand conservation failed: expected %u, got %u\n",
              brush_cells * 2u, conserved_sand_count);
      exit_code = 5;
      goto done;
    }

    ClearSimulation(&application);
    if (!UpdateConstants(&application) ||
        !ApplyTestPixel(&application, SIM_PIXEL_WATER, 199, 200, 3u) ||
        !ApplyTestPixel(&application, SIM_PIXEL_SAND, 199, 199, 4u) ||
        !ApplyTestPixel(&application, SIM_PIXEL_IRON, 199, 201, 5u)) {
      exit_code = 6;
      goto done;
    }
    if (ReadStatePixel(&application, 199, 199) != SIM_PIXEL_SAND ||
        ReadStatePixel(&application, 199, 200) != SIM_PIXEL_WATER ||
        ReadStatePixel(&application, 199, 201) != SIM_PIXEL_IRON ||
        ReadStatePixel(&application, 200, 199) != SIM_PIXEL_EMPTY ||
        ReadStatePixel(&application, 200, 200) != SIM_PIXEL_EMPTY) {
      exit_code = 18;
      goto done;
    }
    if (!RunSimulationUpdate(&application)) {
      exit_code = 6;
      goto done;
    }
    if (ReadStatePixel(&application, 199, 199) != SIM_PIXEL_WATER ||
        ReadStatePixel(&application, 199, 200) != SIM_PIXEL_SAND ||
        ReadStatePixel(&application, 199, 201) != SIM_PIXEL_IRON ||
        CountOccupiedPixels(&application) != 3u) {
      exit_code = 7;
      goto done;
    }

    ClearSimulation(&application);
    if (!UpdateConstants(&application) ||
        !ApplyTestPixel(&application, SIM_PIXEL_WATER, 100, 99, 7u)) {
      exit_code = 6;
      goto done;
    }
    if (ReadStatePixel(&application, 100, 99) != SIM_PIXEL_WATER ||
        ReadStatePixel(&application, 100, 100) != SIM_PIXEL_EMPTY ||
        ReadStatePixel(&application, 99, 100) != SIM_PIXEL_EMPTY) {
      exit_code = 18;
      goto done;
    }
    if (!RunSimulationUpdate(&application)) {
      exit_code = 6;
      goto done;
    }
    if (ReadStatePixel(&application, 100, 99) != SIM_PIXEL_EMPTY ||
        ReadStatePixel(&application, 100, 100) != SIM_PIXEL_WATER) {
      exit_code = 9;
      goto done;
    }

    // Vertical velocity accelerates over consecutive updates.
    ClearSimulation(&application);
    if (!UpdateConstants(&application) ||
        !ApplyTestPixel(&application, SIM_PIXEL_WATER, 300, 49, 10u)) {
      exit_code = 6;
      goto done;
    }
    if (ReadStatePixel(&application, 300, 49) != SIM_PIXEL_WATER ||
        ReadStatePixel(&application, 300, 50) != SIM_PIXEL_EMPTY) {
      exit_code = 18;
      goto done;
    }
    for (uint32_t phase = 0; phase < 4; ++phase) {
      if (!RunSimulationUpdate(&application)) {
        exit_code = 6;
        goto done;
      }
    }
    if (ReadStatePixel(&application, 300, 49) != SIM_PIXEL_EMPTY ||
        ReadStatePixel(&application, 300, 59) != SIM_PIXEL_WATER ||
        CountOccupiedPixels(&application) != 1u) {
      exit_code = 17;
      goto done;
    }

    // Free-falling powder uses the same vertical acceleration as liquid so
    // the two material families remain visually synchronized.
    ClearSimulation(&application);
    if (!UpdateConstants(&application) ||
        !ApplyTestPixel(&application, SIM_PIXEL_SAND, 320, 49, 42u)) {
      exit_code = 6;
      goto done;
    }
    for (uint32_t update = 0; update < 4; ++update) {
      if (!RunSimulationUpdate(&application)) {
        exit_code = 6;
        goto done;
      }
    }
    if (ReadStatePixel(&application, 320, 49) != SIM_PIXEL_EMPTY ||
        ReadStatePixel(&application, 320, 59) != SIM_PIXEL_SAND ||
        CountOccupiedPixels(&application) != 1u) {
      exit_code = 25;
      goto done;
    }

    ClearSimulation(&application);
    if (!UpdateConstants(&application) ||
        !ApplyTestPixel(&application, SIM_PIXEL_SAND, 200, 100, 11u)) {
      exit_code = 6;
      goto done;
    }
    application.frame_index = 1;
    const uint32_t occupied_before_gravity = CountOccupiedPixels(&application);
    if (ReadStatePixel(&application, 200, 100) != SIM_PIXEL_SAND ||
        ReadStatePixel(&application, 200, 101) != SIM_PIXEL_EMPTY ||
        ReadStatePixel(&application, 199, 101) != SIM_PIXEL_EMPTY) {
      exit_code = 18;
      goto done;
    }
    if (!RunSimulationUpdate(&application)) {
      exit_code = 6;
      goto done;
    }
    if (ReadStatePixel(&application, 200, 100) != SIM_PIXEL_EMPTY ||
        ReadStatePixel(&application, 200, 101) != SIM_PIXEL_SAND ||
        CountOccupiedPixels(&application) != occupied_before_gravity) {
      exit_code = 11;
      goto done;
    }

    // A liquid column must keep falling before any cell spreads sideways.
    ClearSimulation(&application);
    if (!UpdateConstants(&application) ||
        !ApplyTestPixel(&application, SIM_PIXEL_WATER, 100, 100, 12u) ||
        !ApplyTestPixel(&application, SIM_PIXEL_WATER, 100, 101, 13u)) {
      exit_code = 6;
      goto done;
    }
    if (ReadStatePixel(&application, 100, 100) != SIM_PIXEL_WATER ||
        ReadStatePixel(&application, 100, 101) != SIM_PIXEL_WATER ||
        ReadStatePixel(&application, 100, 102) != SIM_PIXEL_EMPTY ||
        ReadStatePixel(&application, 99, 100) != SIM_PIXEL_EMPTY ||
        ReadStatePixel(&application, 101, 100) != SIM_PIXEL_EMPTY) {
      exit_code = 18;
      goto done;
    }
    if (!RunSimulationUpdate(&application)) {
      exit_code = 6;
      goto done;
    }
    if (ReadStatePixel(&application, 100, 100) != SIM_PIXEL_EMPTY ||
        ReadStatePixel(&application, 100, 101) != SIM_PIXEL_WATER ||
        ReadStatePixel(&application, 100, 102) != SIM_PIXEL_WATER ||
        ReadStatePixel(&application, 99, 100) != SIM_PIXEL_EMPTY ||
        ReadStatePixel(&application, 101, 100) != SIM_PIXEL_EMPTY ||
        ReadStatePixel(&application, 99, 101) != SIM_PIXEL_EMPTY ||
        ReadStatePixel(&application, 101, 101) != SIM_PIXEL_EMPTY ||
        ReadStatePixel(&application, 99, 102) != SIM_PIXEL_EMPTY ||
        ReadStatePixel(&application, 101, 102) != SIM_PIXEL_EMPTY ||
        CountOccupiedPixels(&application) != 2u) {
      exit_code = 12;
      goto done;
    }

    // A deeper airborne column is still falling as one body. Liquid stacked
    // above other moving liquid must not be mistaken for grounded pressure.
    ClearSimulation(&application);
    if (!UpdateConstants(&application) ||
        !ApplyTestPixel(&application, SIM_PIXEL_WATER, 140, 100, 38u) ||
        !ApplyTestPixel(&application, SIM_PIXEL_WATER, 140, 101, 39u) ||
        !ApplyTestPixel(&application, SIM_PIXEL_WATER, 140, 102, 40u) ||
        !ApplyTestPixel(&application, SIM_PIXEL_WATER, 140, 103, 41u)) {
      exit_code = 6;
      goto done;
    }
    if (!RunSimulationUpdate(&application)) {
      exit_code = 6;
      goto done;
    }
    if (ReadStatePixel(&application, 140, 100) != SIM_PIXEL_EMPTY ||
        ReadStatePixel(&application, 140, 101) != SIM_PIXEL_WATER ||
        ReadStatePixel(&application, 140, 102) != SIM_PIXEL_WATER ||
        ReadStatePixel(&application, 140, 103) != SIM_PIXEL_WATER ||
        ReadStatePixel(&application, 140, 104) != SIM_PIXEL_WATER ||
        ReadStatePixel(&application, 132, 101) != SIM_PIXEL_EMPTY ||
        ReadStatePixel(&application, 148, 101) != SIM_PIXEL_EMPTY ||
        ReadStatePixel(&application, 132, 102) != SIM_PIXEL_EMPTY ||
        ReadStatePixel(&application, 148, 102) != SIM_PIXEL_EMPTY ||
        CountOccupiedPixels(&application) != 4u) {
      exit_code = 24;
      goto done;
    }

    // A deep liquid column must create lateral pressure instead of remaining
    // a rigid stack above its bottom layer.
    ClearSimulation(&application);
    if (!UpdateConstants(&application)) {
      exit_code = 6;
      goto done;
    }
    for (int x = 90; x <= 94; ++x) {
      if (!ApplyTestPixel(&application, SIM_PIXEL_IRON, x, 101,
                          (uint32_t)x)) {
        exit_code = 6;
        goto done;
      }
    }
    if (!ApplyTestPixel(&application, SIM_PIXEL_WATER, 92, 98, 27u) ||
        !ApplyTestPixel(&application, SIM_PIXEL_WATER, 92, 99, 28u) ||
        !ApplyTestPixel(&application, SIM_PIXEL_WATER, 92, 100, 29u)) {
      exit_code = 6;
      goto done;
    }
    if (ReadStatePixel(&application, 92, 98) != SIM_PIXEL_WATER ||
        ReadStatePixel(&application, 91, 98) != SIM_PIXEL_EMPTY ||
        ReadStatePixel(&application, 92, 99) != SIM_PIXEL_WATER ||
        ReadStatePixel(&application, 92, 100) != SIM_PIXEL_WATER ||
        CountOccupiedPixels(&application) != 8u) {
      exit_code = 18;
      goto done;
    }
    if (!RunSimulationUpdate(&application)) {
      exit_code = 6;
      goto done;
    }
    uint32_t pressure_x = 0;
    const bool found_pressure_pixel = FindSinglePixelInRow(
        &application, SIM_PIXEL_WATER, 98, 84, 100, &pressure_x);
    const uint32_t pressure_count = CountOccupiedPixels(&application);
    if (!found_pressure_pixel || pressure_x == 92 || pressure_count != 8u) {
      fprintf(stderr,
              "deep pressure failed: found=%u x=%u occupied=%u\n",
              found_pressure_pixel ? 1u : 0u, pressure_x, pressure_count);
      exit_code = 19;
      goto done;
    }

    // Both diagonal directions must become reachable during one phase cycle.
    ClearSimulation(&application);
    if (!UpdateConstants(&application) ||
        !ApplyTestPixel(&application, SIM_PIXEL_SAND, 100, 100, 14u) ||
        !ApplyTestPixel(&application, SIM_PIXEL_IRON, 100, 101, 15u) ||
        !ApplyTestPixel(&application, SIM_PIXEL_IRON, 101, 101, 16u) ||
        !ApplyTestPixel(&application, SIM_PIXEL_SAND, 201, 100, 17u) ||
        !ApplyTestPixel(&application, SIM_PIXEL_IRON, 201, 101, 18u) ||
        !ApplyTestPixel(&application, SIM_PIXEL_IRON, 200, 101, 19u)) {
      exit_code = 6;
      goto done;
    }
    if (ReadStatePixel(&application, 100, 100) != SIM_PIXEL_SAND ||
        ReadStatePixel(&application, 100, 101) != SIM_PIXEL_IRON ||
        ReadStatePixel(&application, 101, 101) != SIM_PIXEL_IRON ||
        ReadStatePixel(&application, 99, 101) != SIM_PIXEL_EMPTY ||
        ReadStatePixel(&application, 201, 100) != SIM_PIXEL_SAND ||
        ReadStatePixel(&application, 201, 101) != SIM_PIXEL_IRON ||
        ReadStatePixel(&application, 200, 101) != SIM_PIXEL_IRON ||
        ReadStatePixel(&application, 202, 101) != SIM_PIXEL_EMPTY) {
      exit_code = 18;
      goto done;
    }
    for (uint32_t phase = 0; phase < 2; ++phase) {
      if (!RunSimulationUpdate(&application)) {
        exit_code = 6;
        goto done;
      }
    }
    if (ReadStatePixel(&application, 100, 100) != SIM_PIXEL_EMPTY ||
        ReadStatePixel(&application, 99, 101) != SIM_PIXEL_EMPTY ||
        ReadStatePixel(&application, 99, 102) != SIM_PIXEL_SAND) {
      exit_code = 13;
      goto done;
    }
    if (ReadStatePixel(&application, 201, 100) != SIM_PIXEL_EMPTY ||
        ReadStatePixel(&application, 202, 101) != SIM_PIXEL_EMPTY ||
        ReadStatePixel(&application, 202, 102) != SIM_PIXEL_SAND) {
      exit_code = 14;
      goto done;
    }

    // A single supported liquid cell has no hydrostatic pressure and must
    // remain settled instead of moving sideways forever.
    ClearSimulation(&application);
    if (!UpdateConstants(&application)) {
      exit_code = 6;
      goto done;
    }
    for (int x = 80; x <= 120; ++x) {
      if (!ApplyTestPixel(&application, SIM_PIXEL_IRON, x, 101,
                          (uint32_t)x)) {
        exit_code = 6;
        goto done;
      }
    }
    if (!ApplyTestPixel(&application, SIM_PIXEL_IRON, 79, 100, 20u) ||
        !ApplyTestPixel(&application, SIM_PIXEL_IRON, 121, 100, 21u) ||
        !ApplyTestPixel(&application, SIM_PIXEL_WATER, 100, 100, 22u)) {
      exit_code = 6;
      goto done;
    }
    if (ReadStatePixel(&application, 100, 100) != SIM_PIXEL_WATER ||
        ReadStatePixel(&application, 99, 100) != SIM_PIXEL_EMPTY ||
        ReadStatePixel(&application, 101, 100) != SIM_PIXEL_EMPTY ||
        ReadStatePixel(&application, 80, 101) != SIM_PIXEL_IRON ||
        ReadStatePixel(&application, 100, 101) != SIM_PIXEL_IRON ||
        ReadStatePixel(&application, 120, 101) != SIM_PIXEL_IRON ||
        ReadStatePixel(&application, 79, 100) != SIM_PIXEL_IRON ||
        ReadStatePixel(&application, 121, 100) != SIM_PIXEL_IRON) {
      exit_code = 18;
      goto done;
    }
    for (uint32_t phase = 0; phase < 32; ++phase) {
      if (!RunSimulationUpdate(&application)) {
        exit_code = 6;
        goto done;
      }
      uint32_t water_x = 0;
      if (!FindSinglePixelInRow(&application, SIM_PIXEL_WATER, 100, 80, 120,
                                &water_x) ||
          water_x != 100u) {
        exit_code = 15;
        goto done;
      }
    }
    if (CountOccupiedPixels(&application) != 44u) {
      exit_code = 15;
      goto done;
    }

    // Identical reaction neighborhoods at different hashes must not always
    // favor the first edge.
    ClearSimulation(&application);
    if (!UpdateConstants(&application) ||
        !ApplyTestPixel(&application, SIM_PIXEL_WATER, 39, 39, 21u) ||
        !ApplyTestPixel(&application, SIM_PIXEL_LAVA, 40, 39, 22u) ||
        !ApplyTestPixel(&application, SIM_PIXEL_LAVA, 39, 40, 23u) ||
        !ApplyTestPixel(&application, SIM_PIXEL_WATER, 41, 39, 24u) ||
        !ApplyTestPixel(&application, SIM_PIXEL_LAVA, 42, 39, 25u) ||
        !ApplyTestPixel(&application, SIM_PIXEL_LAVA, 41, 40, 26u)) {
      exit_code = 6;
      goto done;
    }
    if (ReadStatePixel(&application, 39, 39) != SIM_PIXEL_WATER ||
        ReadStatePixel(&application, 40, 39) != SIM_PIXEL_LAVA ||
        ReadStatePixel(&application, 39, 40) != SIM_PIXEL_LAVA ||
        ReadStatePixel(&application, 40, 40) != SIM_PIXEL_EMPTY ||
        ReadStatePixel(&application, 41, 39) != SIM_PIXEL_WATER ||
        ReadStatePixel(&application, 42, 39) != SIM_PIXEL_LAVA ||
        ReadStatePixel(&application, 41, 40) != SIM_PIXEL_LAVA ||
        ReadStatePixel(&application, 42, 40) != SIM_PIXEL_EMPTY) {
      exit_code = 18;
      goto done;
    }
    if (!UpdateConstants(&application)) {
      exit_code = 6;
      goto done;
    }
    Dispatch(&application, application.simulate_shader);
    ++application.frame_index;
    if (ReadStatePixel(&application, 39, 39) != SIM_PIXEL_STEAM ||
        ReadStatePixel(&application, 40, 39) != SIM_PIXEL_IRON ||
        ReadStatePixel(&application, 39, 40) != SIM_PIXEL_LAVA ||
        ReadStatePixel(&application, 41, 39) != SIM_PIXEL_STEAM ||
        ReadStatePixel(&application, 41, 40) != SIM_PIXEL_IRON ||
        ReadStatePixel(&application, 42, 39) != SIM_PIXEL_LAVA ||
        ReadStatePixel(&application, 42, 40) != SIM_PIXEL_EMPTY ||
        CountOccupiedPixels(&application) != 6u) {
      exit_code = 16;
      goto done;
    }

    // Rust propagates into adjacent iron while preserving occupied-cell
    // count. Wood support isolates the intended iron target from the powder.
    ClearSimulation(&application);
    if (!UpdateConstants(&application)) {
      exit_code = 6;
      goto done;
    }
    for (int x = 49; x <= 52; ++x) {
      if (!ApplyTestPixel(&application, SIM_PIXEL_WOOD, x, 51,
                          (uint32_t)(x + 50))) {
        exit_code = 6;
        goto done;
      }
    }
    if (!ApplyTestPixel(&application, SIM_PIXEL_RUST, 50, 50, 43u) ||
        !ApplyTestPixel(&application, SIM_PIXEL_IRON, 51, 50, 44u) ||
        CountOccupiedPixels(&application) != 6u) {
      exit_code = 18;
      goto done;
    }
    for (uint32_t update = 0; update < 256; ++update) {
      if (!RunSimulationUpdate(&application)) {
        exit_code = 6;
        goto done;
      }
    }
    if (ReadStatePixel(&application, 50, 50) != SIM_PIXEL_RUST ||
        ReadStatePixel(&application, 51, 50) != SIM_PIXEL_RUST ||
        CountOccupiedPixels(&application) != 6u) {
      exit_code = 26;
      goto done;
    }

    // New material reactions are checked in isolated blocks before gravity can
    // move their participants away from the intended interface.
    ClearSimulation(&application);
    if (!UpdateConstants(&application) ||
        !ApplyTestPixel(&application, SIM_PIXEL_ACID, 49, 49, 47u) ||
        !ApplyTestPixel(&application, SIM_PIXEL_IRON, 50, 49, 48u) ||
        !ApplyTestPixel(&application, SIM_PIXEL_FIRE, 53, 49, 49u) ||
        !ApplyTestPixel(&application, SIM_PIXEL_OIL, 54, 49, 50u) ||
        !ApplyTestPixel(&application, SIM_PIXEL_SALT, 57, 49, 51u) ||
        !ApplyTestPixel(&application, SIM_PIXEL_WATER, 58, 49, 52u) ||
        !ApplyTestPixel(&application, SIM_PIXEL_ACID, 61, 49, 53u) ||
        !ApplyTestPixel(&application, SIM_PIXEL_BEDROCK, 62, 49, 54u) ||
        !ApplyTestPixel(&application, SIM_PIXEL_ACID, 65, 49, 55u) ||
        !ApplyTestPixel(&application, SIM_PIXEL_SALT, 66, 49, 56u) ||
        !ApplyTestPixel(&application, SIM_PIXEL_FIRE, 69, 49, 57u) ||
        !ApplyTestPixel(&application, SIM_PIXEL_WATER, 70, 49, 58u)) {
      exit_code = 6;
      goto done;
    }
    Dispatch(&application, application.simulate_shader);
    if (ReadStatePixel(&application, 49, 49) != SIM_PIXEL_ACID ||
        ReadStatePixel(&application, 50, 49) != SIM_PIXEL_EMPTY ||
        ReadStatePixel(&application, 54, 49) != SIM_PIXEL_FIRE ||
        ReadStatePixel(&application, 57, 49) != SIM_PIXEL_SALT ||
        ReadStatePixel(&application, 58, 49) != SIM_PIXEL_WATER ||
        ReadStatePixel(&application, 61, 49) != SIM_PIXEL_ACID ||
        ReadStatePixel(&application, 62, 49) != SIM_PIXEL_BEDROCK ||
        ReadStatePixel(&application, 65, 49) != SIM_PIXEL_ACID ||
        ReadStatePixel(&application, 66, 49) != SIM_PIXEL_SALT ||
        ReadStatePixel(&application, 69, 49) != SIM_PIXEL_STEAM) {
      fprintf(stderr, "new material reaction verification failed\n");
      exit_code = 30;
      goto done;
    }

    // Salt remains granular on initial contact, then gradually transfers into
    // a neighboring water cell as persistent dissolved-salt metadata.
    ClearSimulation(&application);
    if (!UpdateConstants(&application) ||
        !ApplyTestPixel(&application, SIM_PIXEL_SALT, 81, 87, 69u) ||
        !ApplyTestPixel(&application, SIM_PIXEL_WATER, 82, 87, 70u)) {
      exit_code = 6;
      goto done;
    }
    for (int x = 80; x <= 83; ++x) {
      if (!ApplyTestPixel(&application, SIM_PIXEL_BEDROCK, x, 88,
                          (uint32_t)(x + 71))) {
        exit_code = 6;
        goto done;
      }
    }
    if (!RunSimulationUpdate(&application) ||
        ReadStatePixel(&application, 81, 87) != SIM_PIXEL_SALT ||
        (ReadStateValue(&application, 82, 87) & SIM_DISSOLVED_SALT_MASK) != 0u) {
      fprintf(stderr, "salt dissolved immediately on water contact\n");
      exit_code = 33;
      goto done;
    }
    for (uint32_t update = 1; update < 64u; ++update) {
      if (!RunSimulationUpdate(&application)) {
        exit_code = 6;
        goto done;
      }
    }
    if (ReadStatePixel(&application, 81, 87) != SIM_PIXEL_EMPTY ||
        ReadStatePixel(&application, 82, 87) != SIM_PIXEL_WATER ||
        (ReadStateValue(&application, 82, 87) & SIM_DISSOLVED_SALT_MASK) == 0u) {
      fprintf(stderr, "gradual salt dissolution verification failed\n");
      exit_code = 34;
      goto done;
    }

    // Water is denser than oil, so an oil layer rises when water falls into it.
    ClearSimulation(&application);
    if (!UpdateConstants(&application) ||
        !ApplyTestPixel(&application, SIM_PIXEL_WATER, 100, 99, 59u) ||
        !ApplyTestPixel(&application, SIM_PIXEL_OIL, 100, 100, 60u) ||
        !ApplyTestPixel(&application, SIM_PIXEL_BEDROCK, 99, 100, 61u) ||
        !ApplyTestPixel(&application, SIM_PIXEL_BEDROCK, 101, 100, 62u) ||
        !ApplyTestPixel(&application, SIM_PIXEL_BEDROCK, 100, 101, 63u) ||
        !RunSimulationUpdate(&application) ||
        ReadStatePixel(&application, 100, 99) != SIM_PIXEL_OIL ||
        ReadStatePixel(&application, 100, 100) != SIM_PIXEL_WATER) {
      fprintf(stderr, "oil buoyancy verification failed\n");
      exit_code = 31;
      goto done;
    }

    // Fire has a bounded lifetime and leaves smoke when it cannot spread.
    ClearSimulation(&application);
    if (!UpdateConstants(&application) ||
        !ApplyTestPixel(&application, SIM_PIXEL_FIRE, 100, 100, 64u) ||
        !ApplyTestPixel(&application, SIM_PIXEL_BEDROCK, 100, 99, 65u) ||
        !ApplyTestPixel(&application, SIM_PIXEL_BEDROCK, 99, 100, 66u) ||
        !ApplyTestPixel(&application, SIM_PIXEL_BEDROCK, 101, 100, 67u) ||
        !ApplyTestPixel(&application, SIM_PIXEL_BEDROCK, 100, 101, 68u)) {
      exit_code = 6;
      goto done;
    }
    for (uint32_t update = 0; update < 46u; ++update) {
      if (!RunSimulationUpdate(&application)) {
        exit_code = 6;
        goto done;
      }
    }
    if (ReadStatePixel(&application, 100, 100) != SIM_PIXEL_SMOKE ||
        CountOccupiedPixels(&application) != 5u) {
      fprintf(stderr, "fire lifetime verification failed\n");
      exit_code = 32;
      goto done;
    }

    // Velocity accelerates liquid beyond one cell per update, but every cell
    // on the path is checked so the particle stops immediately above a wall.
    ClearSimulation(&application);
    if (!UpdateConstants(&application) ||
        !ApplyTestPixel(&application, SIM_PIXEL_WATER, 100, 20, 32u) ||
        !ApplyTestPixel(&application, SIM_PIXEL_IRON, 100, 25, 33u) ||
        !ApplyTestPixel(&application, SIM_PIXEL_IRON, 99, 24, 34u) ||
        !ApplyTestPixel(&application, SIM_PIXEL_IRON, 101, 24, 35u)) {
      exit_code = 6;
      goto done;
    }
    if (ReadStatePixel(&application, 100, 20) != SIM_PIXEL_WATER ||
        ReadStatePixel(&application, 100, 21) != SIM_PIXEL_EMPTY ||
        ReadStatePixel(&application, 100, 24) != SIM_PIXEL_EMPTY ||
        ReadStatePixel(&application, 100, 25) != SIM_PIXEL_IRON ||
        ReadStatePixel(&application, 99, 24) != SIM_PIXEL_IRON ||
        ReadStatePixel(&application, 101, 24) != SIM_PIXEL_IRON) {
      exit_code = 18;
      goto done;
    }
    for (uint32_t update = 0; update < 3; ++update) {
      if (!RunSimulationUpdate(&application)) {
        exit_code = 6;
        goto done;
      }
    }
    if (ReadStatePixel(&application, 100, 20) != SIM_PIXEL_EMPTY ||
        ReadStatePixel(&application, 100, 24) != SIM_PIXEL_WATER ||
        ReadStatePixel(&application, 100, 25) != SIM_PIXEL_IRON ||
        CountOccupiedPixels(&application) != 4u) {
      exit_code = 21;
      goto done;
    }

    // Accelerated lava must react with the first crossed water cell instead
    // of tunneling through it to an empty destination farther below.
    ClearSimulation(&application);
    if (!UpdateConstants(&application) ||
        !ApplyTestPixel(&application, SIM_PIXEL_LAVA, 180, 20, 45u)) {
      exit_code = 6;
      goto done;
    }
    for (uint32_t update = 0; update < 2; ++update) {
      if (!RunSimulationUpdate(&application)) {
        exit_code = 6;
        goto done;
      }
    }
    if (ReadStatePixel(&application, 180, 23) != SIM_PIXEL_LAVA ||
        !ApplyTestPixel(&application, SIM_PIXEL_WATER, 180, 25, 46u) ||
        ReadStatePixel(&application, 180, 24) != SIM_PIXEL_EMPTY ||
        ReadStatePixel(&application, 180, 26) != SIM_PIXEL_EMPTY) {
      exit_code = 18;
      goto done;
    }
    if (!RunSimulationUpdate(&application)) {
      exit_code = 6;
      goto done;
    }
    if (ReadStatePixel(&application, 180, 25) != SIM_PIXEL_LAVA ||
        ReadStatePixel(&application, 180, 26) != SIM_PIXEL_WATER) {
      fprintf(stderr, "fast lava crossed a reactive water layer\n");
      exit_code = 29;
      goto done;
    }
    for (uint32_t reaction_update = 0; reaction_update < 2u;
         ++reaction_update) {
      if (!RunSimulationUpdate(&application)) {
        exit_code = 6;
        goto done;
      }
    }
    uint32_t reaction_iron = 0;
    uint32_t reaction_steam = 0;
    uint32_t reaction_lava = 0;
    uint32_t reaction_water = 0;
    uint32_t reaction_min_x = 0;
    uint32_t reaction_max_x = 0;
    uint32_t reaction_min_y = 0;
    uint32_t reaction_max_y = 0;
    uint64_t reaction_checksum = 0;
    if (!MeasureMaterial(&application, SIM_PIXEL_IRON, &reaction_iron,
                         &reaction_min_x, &reaction_max_x, &reaction_min_y,
                         &reaction_max_y, &reaction_checksum) ||
        !MeasureMaterial(&application, SIM_PIXEL_STEAM, &reaction_steam,
                         &reaction_min_x, &reaction_max_x, &reaction_min_y,
                         &reaction_max_y, &reaction_checksum) ||
        !MeasureMaterial(&application, SIM_PIXEL_LAVA, &reaction_lava,
                         &reaction_min_x, &reaction_max_x, &reaction_min_y,
                         &reaction_max_y, &reaction_checksum) ||
        !MeasureMaterial(&application, SIM_PIXEL_WATER, &reaction_water,
                         &reaction_min_x, &reaction_max_x, &reaction_min_y,
                         &reaction_max_y, &reaction_checksum)) {
      exit_code = 6;
      goto done;
    }
    const uint32_t reaction_count = CountOccupiedPixels(&application);
    if (reaction_iron != 1u || reaction_steam != 1u || reaction_lava != 0u ||
        reaction_water != 0u ||
        reaction_count != 2u) {
      fprintf(stderr,
              "fast lava reaction failed: iron=%u steam=%u lava=%u water=%u count=%u\n",
              reaction_iron, reaction_steam, reaction_lava, reaction_water,
              reaction_count);
      exit_code = 29;
      goto done;
    }

    // Pressure transfer stops at internal walls rather than crossing them.
    ClearSimulation(&application);
    if (!UpdateConstants(&application)) {
      exit_code = 6;
      goto done;
    }
    for (int x = 80; x <= 120; ++x) {
      if (!ApplyTestPixel(&application, SIM_PIXEL_IRON, x, 101,
                          (uint32_t)x)) {
        exit_code = 6;
        goto done;
      }
    }
    if (!ApplyTestPixel(&application, SIM_PIXEL_IRON, 105, 99, 36u) ||
        !ApplyTestPixel(&application, SIM_PIXEL_WATER, 100, 100, 37u) ||
        !ApplyTestPixel(&application, SIM_PIXEL_WATER, 100, 99, 38u) ||
        CountOccupiedPixels(&application) != 44u) {
      exit_code = 18;
      goto done;
    }
    if (!RunSimulationUpdate(&application)) {
      exit_code = 6;
      goto done;
    }
    if (ReadStatePixel(&application, 100, 100) != SIM_PIXEL_WATER ||
        ReadStatePixel(&application, 100, 99) != SIM_PIXEL_EMPTY ||
        ReadStatePixel(&application, 99, 99) != SIM_PIXEL_WATER ||
        ReadStatePixel(&application, 105, 99) != SIM_PIXEL_IRON ||
        ReadStatePixel(&application, 106, 99) != SIM_PIXEL_EMPTY ||
        CountOccupiedPixels(&application) != 44u) {
      exit_code = 23;
      goto done;
    }

    // A bulk liquid deposit must settle into a broad, shallow pool rather
    // than retaining the tall mound produced by one-cell powder-like flow.
    ClearSimulation(&application);
    if (!UpdateConstants(&application)) {
      exit_code = 6;
      goto done;
    }
    for (int x = 250; x <= 349; ++x) {
      if (!ApplyTestPixel(&application, SIM_PIXEL_IRON, x, 300,
                          (uint32_t)x)) {
        exit_code = 6;
        goto done;
      }
    }
    for (int y = 270; y < 300; ++y) {
      if (!ApplyTestPixel(&application, SIM_PIXEL_IRON, 250, y,
                          (uint32_t)y) ||
          !ApplyTestPixel(&application, SIM_PIXEL_IRON, 349, y,
                          (uint32_t)(y + 31))) {
        exit_code = 6;
        goto done;
      }
    }
    for (int y = 250; y < 266; ++y) {
      for (int x = 291; x < 307; ++x) {
        if (!ApplyTestPixel(&application, SIM_PIXEL_WATER, x, y,
                            (uint32_t)(x * 397 + y))) {
          exit_code = 6;
          goto done;
        }
      }
    }
    if (CountOccupiedPixels(&application) != 416u) {
      exit_code = 18;
      goto done;
    }
    for (uint32_t update = 0; update < 256; ++update) {
      if (!RunSimulationUpdate(&application)) {
        exit_code = 6;
        goto done;
      }
    }
    uint32_t water_count = 0;
    uint32_t water_min_x = 0;
    uint32_t water_max_x = 0;
    uint32_t water_min_y = 0;
    uint32_t water_max_y = 0;
    uint64_t settled_checksum = 0;
    if (!MeasureMaterial(&application, SIM_PIXEL_WATER, &water_count,
                         &water_min_x, &water_max_x, &water_min_y,
                         &water_max_y, &settled_checksum)) {
      exit_code = 220;
      goto done;
    }
    if (water_count != 256u) {
      exit_code = 221;
      goto done;
    }
    if (water_max_x - water_min_x + 1u < 80u) {
      fprintf(stderr,
              "basin width failed: min_x=%u max_x=%u width=%u water=%u\n",
              water_min_x, water_max_x, water_max_x - water_min_x + 1u,
              water_count);
      exit_code = 222;
      goto done;
    }
    if (water_min_y < 295u || water_max_y != 299u) {
      exit_code = 22;
      goto done;
    }
    if (CountOccupiedPixels(&application) != 416u) {
      exit_code = 224;
      goto done;
    }
    uint32_t shallowest_depth = 0;
    uint32_t deepest_depth = 0;
    uint32_t basin_holes = 0;
    if (!MeasureBasinSurface(&application, SIM_PIXEL_WATER, 251, 348, 300,
                             &shallowest_depth, &deepest_depth,
                             &basin_holes)) {
      exit_code = 225;
      goto done;
    }
    if (deepest_depth - shallowest_depth > 1u || basin_holes != 0u) {
      exit_code = 28;
      goto done;
    }
    for (uint32_t update = 0; update < 64; ++update) {
      if (!RunSimulationUpdate(&application)) {
        exit_code = 6;
        goto done;
      }
    }
    uint64_t resting_checksum = 0;
    if (!MeasureMaterial(&application, SIM_PIXEL_WATER, &water_count,
                         &water_min_x, &water_max_x, &water_min_y,
                         &water_max_y, &resting_checksum) ||
        resting_checksum != settled_checksum) {
      exit_code = 27;
      goto done;
    }
    double gpu_milliseconds = 0.0;
    if (!MeasureSimulationGpuMilliseconds(&application, 120u,
                                          &gpu_milliseconds)) {
      exit_code = 226;
      goto done;
    }
    fprintf(stderr, "average GPU simulation update: %.3f ms\n",
            gpu_milliseconds);
    if (gpu_milliseconds > 1000.0 / (double)SIM_UPDATES_PER_SECOND) {
      exit_code = 227;
      goto done;
    }
    if (!Render(&application)) {
      exit_code = 228;
      goto done;
    }
    goto done;
  }
  MSG message;
  while (true) {
    while (PeekMessage(&message, NULL, 0, 0, PM_REMOVE)) {
      if (message.message == WM_QUIT) {
        exit_code = (int)message.wParam;
        goto done;
      }
      TranslateMessage(&message);
      DispatchMessage(&message);
    }
    if (!Render(&application)) {
      exit_code = 6;
      goto done;
    }
  }
done:
  ReleasePostProcessTargets(&application);
  Release(application.text.blend);
  Release(application.text.sampler);
  Release(application.text.input_layout);
  Release(application.text.pixel_shader);
  Release(application.text.vertex_shader);
  Release(application.text.vertices);
  Release(application.text.atlas_srv);
  Release(application.text.atlas);
  Release(application.point_sampler);
  Release(application.linear_sampler);
  Release(application.bloom_composite_shader);
  Release(application.bloom_blur_vertical_shader);
  Release(application.bloom_blur_horizontal_shader);
  Release(application.bloom_emission_shader);
  Release(application.pixel_shader);
  Release(application.vertex_shader);
  Release(application.brush_shader);
  Release(application.liquid_horizontal_shader);
  Release(application.falling_vertical_shader);
  Release(application.simulate_shader);
  Release(application.constants);
  Release(application.brush_srv);
  Release(application.brush_buffer);
  Release(application.state_uav[0]);
  Release(application.state_uav[1]);
  Release(application.state_srv[0]);
  Release(application.state_srv[1]);
  Release(application.state[0]);
  Release(application.state[1]);
  Release(application.backbuffer);
  Release(application.swap_chain);
  Release(application.context);
  Release(application.device);
  return exit_code;
}
