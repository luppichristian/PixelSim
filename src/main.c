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

#include "sim.h"
#include "sim_brush.h"
#include "sim_clock.h"
#include "sim_palette.h"
#include "sim_viewport.h"

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

static void release(void* object) {
  if (object != NULL)
    IUnknown_Release((IUnknown*)object);
}

static bool check(HRESULT result, const char* where) {
  if (FAILED(result)) {
    char message[128];
    snprintf(message, sizeof(message), "%s failed: 0x%08lx", where, (unsigned long)result);
    MessageBoxA(NULL, message, "PixelSim", MB_ICONERROR);
    return false;
  }
  return true;
}

static bool compile_shader(const char* entry, const char* target, ID3DBlob** blob) {
  ID3DBlob* errors = NULL;
  HRESULT result = D3DCompileFromFile(L"assets/shaders/pixelsim.hlsl", NULL,
                                      D3D_COMPILE_STANDARD_FILE_INCLUDE, entry, target,
                                      D3DCOMPILE_ENABLE_STRICTNESS, 0, blob, &errors);
  if (FAILED(result)) {
    const char* text = errors ? (const char*)ID3D10Blob_GetBufferPointer(errors) : "shader file not found";
    fprintf(stderr, "shader %s compilation failed:\n%s\n", entry, text);
    MessageBoxA(NULL, text, "PixelSim shader compilation", MB_ICONERROR);
    release(errors);
    return false;
  }
  release(errors);
  return true;
}

static bool load_ui_font(uint8_t** font_data) {
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

static bool create_text_resources(app* application) {
  uint8_t* font_data = NULL;
  uint8_t* atlas_data = calloc(FONT_ATLAS_SIZE, FONT_ATLAS_SIZE);
  if (!atlas_data || !load_ui_font(&font_data)) {
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
  if (!check(atlas_result, "CreateTexture2D font atlas") ||
      !check(ID3D11Device_CreateShaderResourceView(
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
  if (!check(ID3D11Device_CreateBuffer(application->device, &vertices, NULL,
                                       &application->text.vertices),
             "CreateBuffer text vertices"))
    return false;

  ID3DBlob* blob = NULL;
  if (!compile_shader("text_vertex", "vs_5_0", &blob))
    return false;
  bool created = check(ID3D11Device_CreateVertexShader(
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
    created = check(ID3D11Device_CreateInputLayout(
                        application->device, layout,
                        sizeof(layout) / sizeof(layout[0]),
                        ID3D10Blob_GetBufferPointer(blob),
                        ID3D10Blob_GetBufferSize(blob),
                        &application->text.input_layout),
                    "CreateInputLayout text");
  release(blob);
  blob = NULL;
  if (!created || !compile_shader("draw_text", "ps_5_0", &blob))
    return false;
  created = check(ID3D11Device_CreatePixelShader(
                      application->device,
                      ID3D10Blob_GetBufferPointer(blob),
                      ID3D10Blob_GetBufferSize(blob), NULL,
                      &application->text.pixel_shader),
                  "CreatePixelShader text");
  release(blob);
  if (!created)
    return false;

  D3D11_SAMPLER_DESC sampler = {0};
  sampler.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
  sampler.AddressU = sampler.AddressV = sampler.AddressW =
      D3D11_TEXTURE_ADDRESS_CLAMP;
  if (!check(ID3D11Device_CreateSamplerState(
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
  return check(ID3D11Device_CreateBlendState(
                   application->device, &blend, &application->text.blend),
               "CreateBlendState text");
}

static bool create_state_texture(app* application, uint32_t index) {
  D3D11_TEXTURE2D_DESC texture = {0};
  texture.Width = SIM_WIDTH;
  texture.Height = SIM_HEIGHT;
  texture.MipLevels = 1;
  texture.ArraySize = 1;
  texture.Format = DXGI_FORMAT_R32_UINT;
  texture.SampleDesc.Count = 1;
  texture.Usage = D3D11_USAGE_DEFAULT;
  texture.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
  return check(ID3D11Device_CreateTexture2D(application->device, &texture, NULL, &application->state[index]), "CreateTexture2D") &&
         check(ID3D11Device_CreateShaderResourceView(application->device, (ID3D11Resource*)application->state[index], NULL, &application->state_srv[index]), "CreateShaderResourceView") &&
         check(ID3D11Device_CreateUnorderedAccessView(application->device, (ID3D11Resource*)application->state[index], NULL, &application->state_uav[index]), "CreateUnorderedAccessView");
}

static void release_post_process_targets(app* application) {
  for (uint32_t index = 0; index < 2; ++index) {
    release(application->bloom_srv[index]);
    release(application->bloom_target[index]);
    release(application->bloom_texture[index]);
    application->bloom_srv[index] = NULL;
    application->bloom_target[index] = NULL;
    application->bloom_texture[index] = NULL;
  }
  release(application->scene_srv);
  release(application->scene_target);
  release(application->scene_texture);
  application->scene_srv = NULL;
  application->scene_target = NULL;
  application->scene_texture = NULL;
}

static bool create_post_process_texture(app* application,
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
  return check(ID3D11Device_CreateTexture2D(application->device, &description,
                                            NULL, texture),
               "CreateTexture2D post process") &&
         check(ID3D11Device_CreateRenderTargetView(
                   application->device, (ID3D11Resource*)*texture, NULL,
                   target),
               "CreateRenderTargetView post process") &&
         check(ID3D11Device_CreateShaderResourceView(
                   application->device, (ID3D11Resource*)*texture, NULL,
                   resource),
               "CreateShaderResourceView post process");
}

static bool create_post_process_targets(app* application, int width, int height) {
  release_post_process_targets(application);
  const uint32_t bloom_width = (uint32_t)(width > 1 ? width / 2 : 1);
  const uint32_t bloom_height = (uint32_t)(height > 1 ? height / 2 : 1);
  if (!create_post_process_texture(
          application, (uint32_t)width, (uint32_t)height,
          &application->scene_texture, &application->scene_target,
          &application->scene_srv))
    return false;
  for (uint32_t index = 0; index < 2; ++index) {
    if (!create_post_process_texture(
            application, bloom_width, bloom_height,
            &application->bloom_texture[index],
            &application->bloom_target[index],
            &application->bloom_srv[index]))
      return false;
  }
  return true;
}

static bool create_gpu_resources(app* application) {
  D3D11_BUFFER_DESC constants = {0};
  constants.ByteWidth = sizeof(gpu_frame_constants);
  constants.Usage = D3D11_USAGE_DYNAMIC;
  constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  constants.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  if (!check(ID3D11Device_CreateBuffer(application->device, &constants, NULL, &application->constants), "CreateBuffer constants"))
    return false;

  D3D11_BUFFER_DESC brush = {0};
  brush.ByteWidth = sizeof(sim_brush_command);
  brush.Usage = D3D11_USAGE_DYNAMIC;
  brush.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  brush.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  brush.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
  brush.StructureByteStride = sizeof(sim_brush_command);
  if (!check(ID3D11Device_CreateBuffer(application->device, &brush, NULL, &application->brush_buffer), "CreateBuffer brush"))
    return false;
  if (!check(ID3D11Device_CreateShaderResourceView(application->device, (ID3D11Resource*)application->brush_buffer, NULL, &application->brush_srv), "CreateShaderResourceView brush"))
    return false;

  D3D11_SAMPLER_DESC sampler = {0};
  sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
  sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  if (!check(ID3D11Device_CreateSamplerState(application->device, &sampler, &application->point_sampler), "CreateSamplerState"))
    return false;

  sampler.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
  if (!check(ID3D11Device_CreateSamplerState(
                 application->device, &sampler,
                 &application->linear_sampler),
             "CreateSamplerState bloom"))
    return false;

  ID3DBlob* blob = NULL;
  if (!compile_shader("simulate", "cs_5_0", &blob))
    return false;
  bool created = check(ID3D11Device_CreateComputeShader(application->device, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), NULL, &application->simulate_shader), "CreateComputeShader");
  release(blob);
  blob = NULL;
  if (!created ||
      !compile_shader("move_falling_materials_vertical", "cs_5_0", &blob))
    return false;
  created = check(ID3D11Device_CreateComputeShader(
                      application->device,
                      ID3D10Blob_GetBufferPointer(blob),
                      ID3D10Blob_GetBufferSize(blob), NULL,
                      &application->falling_vertical_shader),
                  "CreateComputeShader falling materials vertical");
  release(blob);
  blob = NULL;
  if (!created || !compile_shader("move_liquids_horizontal", "cs_5_0", &blob))
    return false;
  created = check(ID3D11Device_CreateComputeShader(application->device, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), NULL, &application->liquid_horizontal_shader), "CreateComputeShader liquid horizontal");
  release(blob);
  blob = NULL;
  if (!created || !compile_shader("apply_brush", "cs_5_0", &blob))
    return false;
  created = check(ID3D11Device_CreateComputeShader(application->device, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), NULL, &application->brush_shader), "CreateComputeShader brush");
  release(blob);
  blob = NULL;
  if (!created || !compile_shader("fullscreen_vertex", "vs_5_0", &blob))
    return false;
  created = check(ID3D11Device_CreateVertexShader(application->device, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), NULL, &application->vertex_shader), "CreateVertexShader");
  release(blob);
  blob = NULL;
  if (!created || !compile_shader("draw_pixels", "ps_5_0", &blob))
    return false;
  created = check(ID3D11Device_CreatePixelShader(application->device, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), NULL, &application->pixel_shader), "CreatePixelShader");
  release(blob);
  blob = NULL;
  if (!created || !compile_shader("draw_bloom_emission", "ps_5_0", &blob))
    return false;
  created = check(ID3D11Device_CreatePixelShader(
                      application->device,
                      ID3D10Blob_GetBufferPointer(blob),
                      ID3D10Blob_GetBufferSize(blob), NULL,
                      &application->bloom_emission_shader),
                  "CreatePixelShader bloom emission");
  release(blob);
  blob = NULL;
  if (!created || !compile_shader("blur_bloom_horizontal", "ps_5_0", &blob))
    return false;
  created = check(ID3D11Device_CreatePixelShader(
                      application->device,
                      ID3D10Blob_GetBufferPointer(blob),
                      ID3D10Blob_GetBufferSize(blob), NULL,
                      &application->bloom_blur_horizontal_shader),
                  "CreatePixelShader bloom blur horizontal");
  release(blob);
  blob = NULL;
  if (!created || !compile_shader("blur_bloom_vertical", "ps_5_0", &blob))
    return false;
  created = check(ID3D11Device_CreatePixelShader(
                      application->device,
                      ID3D10Blob_GetBufferPointer(blob),
                      ID3D10Blob_GetBufferSize(blob), NULL,
                      &application->bloom_blur_vertical_shader),
                  "CreatePixelShader bloom blur vertical");
  release(blob);
  blob = NULL;
  if (!created || !compile_shader("composite_bloom", "ps_5_0", &blob))
    return false;
  created = check(ID3D11Device_CreatePixelShader(
                      application->device,
                      ID3D10Blob_GetBufferPointer(blob),
                      ID3D10Blob_GetBufferSize(blob), NULL,
                      &application->bloom_composite_shader),
                  "CreatePixelShader bloom composite");
  release(blob);
  if (!created)
    return false;
  if (!create_state_texture(application, 0) || !create_state_texture(application, 1))
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

static bool update_constants(app* application) {
  D3D11_MAPPED_SUBRESOURCE mapped;
  HRESULT result = ID3D11DeviceContext_Map(
      application->context, (ID3D11Resource*)application->constants, 0,
      D3D11_MAP_WRITE_DISCARD, 0, &mapped);
  if (!check(result, "Map frame constants"))
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

static void dispatch(app* application, ID3D11ComputeShader* shader) {
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

static bool dispatch_horizontal_in_place(app* application, uint32_t phase) {
  application->horizontal_phase = phase;
  if (!update_constants(application))
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

static bool run_simulation_update(app* application) {
  ID3D11ComputeShader* ping_pong_passes[] = {
      application->simulate_shader,
      application->falling_vertical_shader,
  };
  _Static_assert(sizeof(ping_pong_passes) / sizeof(ping_pong_passes[0]) == 2u,
                 "the local and vertical passes ping-pong state");

  if (!update_constants(application))
    return false;
  for (uint32_t pass = 0; pass < 2u; ++pass)
    dispatch(application, ping_pong_passes[pass]);
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
    if (!dispatch_horizontal_in_place(application, phase))
      return false;
  }
  ++application->frame_index;
  return true;
}

static bool apply_brush(app* application, sim_brush_command command) {
  D3D11_MAPPED_SUBRESOURCE mapped;
  HRESULT result = ID3D11DeviceContext_Map(
      application->context, (ID3D11Resource*)application->brush_buffer, 0,
      D3D11_MAP_WRITE_DISCARD, 0, &mapped);
  if (!check(result, "Map brush command"))
    return false;
  *(sim_brush_command*)mapped.pData = command;
  ID3D11DeviceContext_Unmap(application->context,
                            (ID3D11Resource*)application->brush_buffer, 0);
  application->brush_bounds = sim_clip_brush_bounds(command);
  if (!update_constants(application))
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

static float text_width(const app* application, const char* text) {
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

static void set_text_vertex(text_vertex* vertex,
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

static void add_text_quad(app* application,
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
  set_text_vertex(&quad[0], x0, y0, u0, v0, color, application->client_width,
                application->client_height);
  set_text_vertex(&quad[1], x1, y0, u1, v0, color, application->client_width,
                application->client_height);
  set_text_vertex(&quad[2], x1, y1, u1, v1, color, application->client_width,
                application->client_height);
  set_text_vertex(&quad[3], x0, y0, u0, v0, color, application->client_width,
                application->client_height);
  set_text_vertex(&quad[4], x1, y1, u1, v1, color, application->client_width,
                application->client_height);
  set_text_vertex(&quad[5], x0, y1, u0, v1, color, application->client_width,
                application->client_height);
  *vertex_count += 6u;
}

static void add_text(app* application,
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
    add_text_quad(application, vertices, vertex_count, quad.x0, quad.y0, quad.x1,
                quad.y1, quad.s0, quad.t0, quad.s1, quad.t1, color);
  }
}

static bool render_tooltip(app* application) {
  if (application->hovered >= SIM_PIXEL_TYPE_COUNT ||
      application->client_width <= 0 || application->client_height <= 0)
    return true;

  char title[64];
  snprintf(title, sizeof(title), "%s  [%c]",
           sim_palette_name(application->hovered),
           sim_palette_shortcut(application->hovered));
  const char* description = sim_palette_description(application->hovered);
  float tooltip_width = text_width(application, title);
  const float description_width = text_width(application, description);
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
  add_text_quad(application, cpu_vertices, &vertex_count, tooltip_x + 3.0f,
              tooltip_y + 3.0f, tooltip_x + tooltip_width + 3.0f,
              tooltip_y + tooltip_height + 3.0f, white_pixel, white_pixel,
              white_pixel, white_pixel, shadow);
  add_text_quad(application, cpu_vertices, &vertex_count, tooltip_x, tooltip_y,
              tooltip_x + tooltip_width, tooltip_y + tooltip_height,
              white_pixel, white_pixel, white_pixel, white_pixel, border);
  add_text_quad(application, cpu_vertices, &vertex_count, tooltip_x + 1.0f,
              tooltip_y + 1.0f, tooltip_x + tooltip_width - 1.0f,
              tooltip_y + tooltip_height - 1.0f, white_pixel, white_pixel,
              white_pixel, white_pixel, background);
  add_text(application, cpu_vertices, &vertex_count, title, tooltip_x + 10.0f,
          tooltip_y + 21.0f, title_color);
  add_text(application, cpu_vertices, &vertex_count, description,
          tooltip_x + 10.0f, tooltip_y + 43.0f, description_color);

  D3D11_MAPPED_SUBRESOURCE mapped;
  if (!check(ID3D11DeviceContext_Map(
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

static void update_fps(app* application);

static void draw_post_process_pass(app* application,
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

static void draw_bloom_emission(app* application,
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

static void render_bloom(app* application) {
  const uint32_t bloom_width =
      (uint32_t)(application->client_width > 1
                     ? application->client_width / 2
                     : 1);
  const uint32_t bloom_height =
      (uint32_t)(application->client_height > 1
                     ? application->client_height / 2
                     : 1);
  draw_bloom_emission(application, bloom_width, bloom_height);
  draw_post_process_pass(application, application->bloom_target[1],
                      application->bloom_blur_horizontal_shader,
                      application->bloom_srv[0], bloom_width, bloom_height);
  draw_post_process_pass(application, application->bloom_target[0],
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

static bool render(app* application) {
  if (application->viewport.width <= 0 || application->viewport.height <= 0) {
    Sleep(10);
    return true;
  }
  if (!update_constants(application))
    return false;
  sim_brush_command command;
  while (sim_brush_queue_pop(&application->brush_queue, &command)) {
    if (!apply_brush(application, command))
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
    if (!run_simulation_update(application))
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
  render_bloom(application);
  if (!render_tooltip(application))
    return false;
  if (!check(IDXGISwapChain_Present(application->swap_chain, 1, 0), "Present"))
    return false;
  update_fps(application);
  return true;
}

static void update_window_title(app* application) {
  char title[128];
  snprintf(title, sizeof(title),
           "PixelSim | %s | FPS: %u | Palette keys: 0-9, A-D",
           sim_palette_name(application->selected), application->fps);
  SetWindowTextA(application->window, title);
}

static void update_fps(app* application) {
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  ++application->fps_frames;
  if (now.QuadPart - application->fps_start.QuadPart < application->fps_frequency.QuadPart)
    return;
  application->fps = (uint32_t)((application->fps_frames * application->fps_frequency.QuadPart) /
                                (now.QuadPart - application->fps_start.QuadPart));
  application->fps_frames = 0;
  application->fps_start = now;
  update_window_title(application);
}

static bool select_material_at_point(app* application, int x, int y) {
  sim_pixel_type material =
      sim_palette_material_at(application->viewport, x, y);
  if (material >= SIM_PIXEL_TYPE_COUNT)
    return false;
  application->selected = material;
  update_window_title(application);
  return true;
}

static bool queue_brush_at_point(app* application, int x, int y) {
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

static bool resize_swap_chain(app* application, int width, int height) {
  application->client_width = width;
  application->client_height = height;
  application->viewport = sim_make_viewport(width, height);
  if (width <= 0 || height <= 0)
    return true;

  ID3D11DeviceContext_OMSetRenderTargets(application->context, 0, NULL, NULL);
  release_post_process_targets(application);
  release(application->backbuffer);
  application->backbuffer = NULL;
  if (!check(IDXGISwapChain_ResizeBuffers(application->swap_chain, 0,
                                          (UINT)width, (UINT)height,
                                          DXGI_FORMAT_UNKNOWN, 0),
             "ResizeBuffers"))
    return false;

  ID3D11Texture2D* backbuffer = NULL;
  if (!check(IDXGISwapChain_GetBuffer(application->swap_chain, 0,
                                      &IID_ID3D11Texture2D,
                                      (void**)&backbuffer),
             "GetBuffer") ||
      !check(ID3D11Device_CreateRenderTargetView(
                 application->device, (ID3D11Resource*)backbuffer, NULL,
                 &application->backbuffer),
             "CreateRenderTargetView")) {
    release(backbuffer);
    return false;
  }
  release(backbuffer);
  if (!create_post_process_targets(application, width, height))
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

static uint32_t count_occupied_pixels(app* application) {
  D3D11_TEXTURE2D_DESC description;
  ID3D11Texture2D_GetDesc(application->state[application->read_state],
                          &description);
  description.Usage = D3D11_USAGE_STAGING;
  description.BindFlags = 0;
  description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  description.MiscFlags = 0;

  ID3D11Texture2D* staging = NULL;
  if (!check(ID3D11Device_CreateTexture2D(application->device, &description,
                                          NULL, &staging),
             "CreateTexture2D verification"))
    return UINT32_MAX;
  ID3D11DeviceContext_CopyResource(
      application->context, (ID3D11Resource*)staging,
      (ID3D11Resource*)application->state[application->read_state]);

  D3D11_MAPPED_SUBRESOURCE mapped;
  if (!check(ID3D11DeviceContext_Map(application->context,
                                     (ID3D11Resource*)staging, 0,
                                     D3D11_MAP_READ, 0, &mapped),
             "Map verification")) {
    release(staging);
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
  release(staging);
  return count;
}

static LRESULT CALLBACK window_procedure(HWND window,
                                        UINT message,
                                        WPARAM wparam,
                                        LPARAM lparam) {
  app* application = (app*)GetWindowLongPtr(window, GWLP_USERDATA);
  switch (message) {
    case WM_LBUTTONDOWN:
      if (!application)
        return 0;
      if (select_material_at_point(application, (int)(short)LOWORD(lparam),
                                (int)(short)HIWORD(lparam)))
        return 0;
      if (queue_brush_at_point(application, (int)(short)LOWORD(lparam),
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
          queue_brush_at_point(application, application->mouse_x,
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
        update_window_title(application);
      } else if (application && wparam >= '1' && wparam <= '9') {
        application->selected = (sim_pixel_type)(wparam - '0');
        update_window_title(application);
      } else if (application && wparam >= 'A' && wparam <= 'D') {
        application->selected = (sim_pixel_type)(SIM_PIXEL_FIRE + wparam - 'A');
        update_window_title(application);
      }
      return 0;
    case WM_SIZE:
      if (application && application->swap_chain &&
          !resize_swap_chain(application, LOWORD(lparam), HIWORD(lparam)))
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
  window_class.lpfnWndProc = window_procedure;
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
  if (!check(D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &swap, &application.swap_chain, &application.device, NULL, &application.context), "D3D11CreateDeviceAndSwapChain"))
    goto done;
  RECT client;
  GetClientRect(application.window, &client);
  if (!resize_swap_chain(&application, client.right, client.bottom))
    goto done;
  if (!create_gpu_resources(&application))
    goto done;
  if (!create_text_resources(&application))
    goto done;
  update_window_title(&application);
  exit_code = 0;
  if (smoke) {
    const uint32_t brush_cells = sim_brush_cell_count(5);
    if (!apply_brush(&application,
                    sim_make_brush_command(SIM_PIXEL_SAND, SIM_WIDTH / 2,
                                           SIM_HEIGHT / 3, 5, 1u)) ||
        count_occupied_pixels(&application) != brush_cells ||
        !apply_brush(&application,
                    sim_make_brush_command(SIM_PIXEL_EMPTY, SIM_WIDTH / 2,
                                           SIM_HEIGHT / 3, 5, 2u)) ||
        count_occupied_pixels(&application) != 0u ||
        !apply_brush(&application,
                    sim_make_brush_command(SIM_PIXEL_SAND, SIM_WIDTH / 2,
                                           SIM_HEIGHT / 3, 5, 3u)) ||
        !apply_brush(&application,
                    sim_make_brush_command(SIM_PIXEL_WATER,
                                           SIM_WIDTH / 2 + 100,
                                           SIM_HEIGHT / 3, 5, 4u))) {
      fprintf(stderr, "GPU brush smoke check failed\n");
      exit_code = 2;
      goto done;
    }
    for (uint32_t update = 0; update < SIM_UPDATES_PER_SECOND; ++update) {
      if (!run_simulation_update(&application)) {
        exit_code = 3;
        goto done;
      }
    }
    if (count_occupied_pixels(&application) != brush_cells * 2u) {
      fprintf(stderr, "GPU simulation conservation smoke check failed\n");
      exit_code = 4;
      goto done;
    }
    if (!render(&application)) {
      exit_code = 5;
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
    if (!render(&application)) {
      exit_code = 6;
      goto done;
    }
  }
done:
  release_post_process_targets(&application);
  release(application.text.blend);
  release(application.text.sampler);
  release(application.text.input_layout);
  release(application.text.pixel_shader);
  release(application.text.vertex_shader);
  release(application.text.vertices);
  release(application.text.atlas_srv);
  release(application.text.atlas);
  release(application.point_sampler);
  release(application.linear_sampler);
  release(application.bloom_composite_shader);
  release(application.bloom_blur_vertical_shader);
  release(application.bloom_blur_horizontal_shader);
  release(application.bloom_emission_shader);
  release(application.pixel_shader);
  release(application.vertex_shader);
  release(application.brush_shader);
  release(application.liquid_horizontal_shader);
  release(application.falling_vertical_shader);
  release(application.simulate_shader);
  release(application.constants);
  release(application.brush_srv);
  release(application.brush_buffer);
  release(application.state_uav[0]);
  release(application.state_uav[1]);
  release(application.state_srv[0]);
  release(application.state_srv[1]);
  release(application.state[0]);
  release(application.state[1]);
  release(application.backbuffer);
  release(application.swap_chain);
  release(application.context);
  release(application.device);
  return exit_code;
}
