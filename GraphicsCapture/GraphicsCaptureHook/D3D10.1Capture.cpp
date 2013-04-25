/********************************************************************************
 Copyright (C) 2012 Hugh Bailey <obs.jim@gmail.com>

 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation; either version 2 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307, USA.
********************************************************************************/


#include "GraphicsCaptureHook.h"

#include <D3D10_1.h>
#include "DXGIStuff.h"


extern HookData         gi1swapResizeBuffers;
extern HookData         gi1swapPresent;
extern FARPROC          oldD3D10Release;
extern FARPROC          newD3D10Release;

CaptureInfo             d3d101CaptureInfo;

extern LPVOID           lpCurrentSwap;
extern LPVOID           lpCurrentDevice;
extern SharedTexData    *texData;
extern DWORD            curCapture;
extern BOOL             bHasTextures;
extern BOOL             bIsMultisampled;
extern LONGLONG         lastTime;

extern DXGI_FORMAT      dxgiFormat;
extern ID3D10Device1    *shareDevice;
ID3D10Resource          *copyD3D101TextureGame = NULL;
extern ID3D10Resource   *copyTextureIntermediary;
extern HANDLE           sharedHandles[2];
extern IDXGIKeyedMutex  *keyedMutexes[2];
extern ID3D10Resource   *sharedTextures[2];


void ClearD3D101Data()
{
    bHasTextures = false;
    if(texData)
        texData->lastRendered = -1;

    for(UINT i=0; i<2; i++)
    {
        SafeRelease(keyedMutexes[i]);
        SafeRelease(sharedTextures[i]);
    }

    SafeRelease(copyD3D101TextureGame);
    SafeRelease(copyTextureIntermediary);
    SafeRelease(shareDevice);

    DestroySharedMemory();
    texData = NULL;
}

void SetupD3D101(IDXGISwapChain *swapChain)
{
    ClearD3D101Data();

    DXGI_SWAP_CHAIN_DESC scd;
    if(SUCCEEDED(swapChain->GetDesc(&scd)))
    {
        d3d101CaptureInfo.format = ConvertGIBackBufferFormat(scd.BufferDesc.Format);
        if(d3d101CaptureInfo.format != GS_UNKNOWNFORMAT)
        {
            if( dxgiFormat          != scd.BufferDesc.Format ||
                d3d101CaptureInfo.cx != scd.BufferDesc.Width  ||
                d3d101CaptureInfo.cy != scd.BufferDesc.Height )
            {
                dxgiFormat = FixCopyTextureFormat(scd.BufferDesc.Format);
                d3d101CaptureInfo.cx = scd.BufferDesc.Width;
                d3d101CaptureInfo.cy = scd.BufferDesc.Height;
                d3d101CaptureInfo.hwndCapture = (DWORD)scd.OutputWindow;
                bIsMultisampled = scd.SampleDesc.Count > 1;
            }
        }
    }

    lastTime = 0;
    OSInitializeTimer();
}


typedef HRESULT (WINAPI *CREATEDXGIFACTORY1PROC)(REFIID riid, void **ppFactory);

bool DoD3D101Hook(ID3D10Device *device)
{
    HRESULT hErr;

    HMODULE hD3D10_1 = GetModuleHandle(TEXT("d3d10_1.dll"));
    if(!hD3D10_1)
        return false;

    HMODULE hDXGI = GetModuleHandle(TEXT("dxgi.dll"));
    if(!hDXGI)
        return false;

    CREATEDXGIFACTORY1PROC createDXGIFactory1 = (CREATEDXGIFACTORY1PROC)GetProcAddress(hDXGI, "CreateDXGIFactory1");
    if(!createDXGIFactory1)
        return false;

    PFN_D3D10_CREATE_DEVICE1 d3d10CreateDevice1 = (PFN_D3D10_CREATE_DEVICE1)GetProcAddress(hD3D10_1, "D3D10CreateDevice1");
    if(!d3d10CreateDevice1)
        return false;

    IDXGIFactory1 *factory;
    if(FAILED(hErr = (*createDXGIFactory1)(__uuidof(IDXGIFactory1), (void**)&factory)))
    {
        RUNONCE logOutput << "DoD3D101Hook: CreateDXGIFactory1 failed, result = " << UINT(hErr) << endl;
        return false;
    }

    IDXGIAdapter1 *adapter;
    if(FAILED(hErr = factory->EnumAdapters1(0, &adapter)))
    {
        RUNONCE logOutput << "DoD3D101Hook: factory->EnumAdapters1 failed, result = " << UINT(hErr) << endl;
        factory->Release();
        return false;
    }

    if(FAILED(hErr = (*d3d10CreateDevice1)(adapter, D3D10_DRIVER_TYPE_HARDWARE, NULL, 0, D3D10_FEATURE_LEVEL_10_1, D3D10_1_SDK_VERSION, &shareDevice)))
    {
        if(FAILED(hErr = (*d3d10CreateDevice1)(adapter, D3D10_DRIVER_TYPE_HARDWARE, NULL, 0, D3D10_FEATURE_LEVEL_9_3, D3D10_1_SDK_VERSION, &shareDevice)))
        {
            RUNONCE logOutput << "DoD3D101Hook: failed to create device, result = " << UINT(hErr) << endl;
            adapter->Release();
            factory->Release();
            return false;
        }
    }

    adapter->Release();
    factory->Release();

    //------------------------------------------------

    D3D10_TEXTURE2D_DESC texGameDesc;
    ZeroMemory(&texGameDesc, sizeof(texGameDesc));
    texGameDesc.Width               = d3d101CaptureInfo.cx;
    texGameDesc.Height              = d3d101CaptureInfo.cy;
    texGameDesc.MipLevels           = 1;
    texGameDesc.ArraySize           = 1;
    texGameDesc.Format              = dxgiFormat;
    texGameDesc.SampleDesc.Count    = 1;
    texGameDesc.BindFlags           = D3D10_BIND_RENDER_TARGET|D3D10_BIND_SHADER_RESOURCE;
    texGameDesc.Usage               = D3D10_USAGE_DEFAULT;
    texGameDesc.MiscFlags           = D3D10_RESOURCE_MISC_SHARED;

    ID3D10Texture2D *d3d101Tex;
    if(FAILED(hErr = device->CreateTexture2D(&texGameDesc, NULL, &d3d101Tex)))
    {
        RUNONCE logOutput << "DoD3D101Hook: failed to create intermediary texture, result = " << UINT(hErr) << endl;
        return false;
    }

    if(FAILED(hErr = d3d101Tex->QueryInterface(__uuidof(ID3D10Resource), (void**)&copyD3D101TextureGame)))
    {
        RUNONCE logOutput << "DoD3D101Hook: d3d101Tex->QueryInterface(ID3D10Resource) failed, result = " << UINT(hErr) << endl;
        d3d101Tex->Release();
        return false;
    }

    IDXGIResource *res;
    if(FAILED(hErr = d3d101Tex->QueryInterface(IID_IDXGIResource, (void**)&res)))
    {
        RUNONCE logOutput << "DoD3D101Hook: d3d101Tex->QueryInterface(IDXGIResource) failed, result = " << UINT(hErr) << endl;
        d3d101Tex->Release();
        return false;
    }

    HANDLE handle;
    if(FAILED(res->GetSharedHandle(&handle)))
    {
        RUNONCE logOutput << "DoD3D101Hook: res->GetSharedHandle failed, result = " << UINT(hErr) << endl;
        d3d101Tex->Release();
        res->Release();
        return false;
    }

    d3d101Tex->Release();
    res->Release();

    //------------------------------------------------

    if(FAILED(hErr = shareDevice->OpenSharedResource(handle, __uuidof(ID3D10Resource), (void**)&copyTextureIntermediary)))
    {
        RUNONCE logOutput << "DoD3D101Hook: shareDevice->OpenSharedResource failed, result = " << UINT(hErr) << endl;
        return false;
    }

    //------------------------------------------------

    D3D10_TEXTURE2D_DESC texDesc;
    ZeroMemory(&texDesc, sizeof(texDesc));
    texDesc.Width               = d3d101CaptureInfo.cx;
    texDesc.Height              = d3d101CaptureInfo.cy;
    texDesc.MipLevels           = 1;
    texDesc.ArraySize           = 1;
    texDesc.Format              = dxgiFormat;
    texDesc.SampleDesc.Count    = 1;
    texDesc.BindFlags           = D3D10_BIND_RENDER_TARGET|D3D10_BIND_SHADER_RESOURCE;
    texDesc.Usage               = D3D10_USAGE_DEFAULT;
    texDesc.MiscFlags           = D3D10_RESOURCE_MISC_SHARED_KEYEDMUTEX;

    for(UINT i=0; i<2; i++)
    {
        ID3D10Texture2D *d3d10tex;
        if(FAILED(hErr = shareDevice->CreateTexture2D(&texDesc, NULL, &d3d10tex)))
        {
            RUNONCE logOutput << "DoD3D101Hook: shareDevice->CreateTexture2D " << i << " failed, result = " << UINT(hErr) << endl;
            return false;
        }

        if(FAILED(hErr = d3d10tex->QueryInterface(__uuidof(ID3D10Resource), (void**)&sharedTextures[i])))
        {
            RUNONCE logOutput << "DoD3D101Hook: d3d10tex->QueryInterface(ID3D10Resource) " << i << " failed, result = " << UINT(hErr) << endl;
            d3d10tex->Release();
            return false;
        }

        if(FAILED(hErr = d3d10tex->QueryInterface(__uuidof(IDXGIKeyedMutex), (void**)&keyedMutexes[i])))
        {
            RUNONCE logOutput << "DoD3D101Hook: d3d10tex->QueryInterface(IDXGIKeyedMutex) " << i << " failed, result = " << UINT(hErr) << endl;
            d3d10tex->Release();
            return false;
        }

        IDXGIResource *res;
        if(FAILED(hErr = d3d10tex->QueryInterface(__uuidof(IDXGIResource), (void**)&res)))
        {
            RUNONCE logOutput << "DoD3D101Hook: d3d10tex->QueryInterface(IDXGIResource) " << i << " failed, result = " << UINT(hErr) << endl;
            d3d10tex->Release();
            return false;
        }

        if(FAILED(hErr = res->GetSharedHandle(&sharedHandles[i])))
        {
            RUNONCE logOutput << "DoD3D101Hook: res->GetSharedHandle " << i << " failed, result = " << UINT(hErr) << endl;
            res->Release();
            d3d10tex->Release();
            return false;
        }

        res->Release();
        d3d10tex->Release();
    }

    return true;
}

void DoD3D101Capture(IDXGISwapChain *swap)
{
    ID3D10Device *device = NULL;
    if(SUCCEEDED(swap->GetDevice(__uuidof(ID3D10Device), (void**)&device)))
    {
        if(!lpCurrentDevice)
        {
            lpCurrentDevice = device;

            /*FARPROC oldRelease = GetVTable(device, (8/4));
            if(oldRelease != newD3D10Release)
            {
                oldD3D10Release = oldRelease;
                newD3D10Release = ConvertClassProcToFarproc((CLASSPROC)&D3D101Override::DeviceReleaseHook);
                SetVTable(device, (8/4), newD3D10Release);
            }*/
        }

        if(bCapturing && bStopRequested)
        {
            ClearD3D101Data();
            bCapturing = false;
            bStopRequested = false;
        }

        if(!bCapturing && WaitForSingleObject(hSignalRestart, 0) == WAIT_OBJECT_0)
        {
            hwndOBS = FindWindow(OBS_WINDOW_CLASS, NULL);
            if(hwndOBS)
                bCapturing = true;
        }

        if(!bHasTextures && bCapturing)
        {
            if(dxgiFormat && hwndOBS)
            {
                BOOL bSuccess = DoD3D101Hook(device);

                if(bSuccess)
                {
                    d3d101CaptureInfo.mapID = InitializeSharedMemoryGPUCapture(&texData);
                    if(!d3d101CaptureInfo.mapID)
                        bSuccess = false;
                }

                if(bSuccess)
                {
                    bHasTextures = true;
                    d3d101CaptureInfo.captureType = CAPTURETYPE_SHAREDTEX;
                    d3d101CaptureInfo.bFlip = FALSE;
                    texData->texHandles[0] = (DWORD)sharedHandles[0];
                    texData->texHandles[1] = (DWORD)sharedHandles[1];

                    memcpy(infoMem, &d3d101CaptureInfo, sizeof(CaptureInfo));
                    SetEvent(hSignalReady);

                    logOutput << "DoD3D101Hook: success" << endl;
                }
                else
                {
                    ClearD3D101Data();
                }
            }
        }

        LONGLONG timeVal = OSGetTimeMicroseconds();

        //check keep alive state, dumb but effective
        if(bCapturing)
        {
            if (!keepAliveTime)
                keepAliveTime = timeVal;

            if((timeVal-keepAliveTime) > 5000000)
            {
                HANDLE hKeepAlive = OpenEvent(EVENT_ALL_ACCESS, FALSE, strKeepAlive.c_str());
                if (hKeepAlive) {
                    CloseHandle(hKeepAlive);
                } else {
                    ClearD3D101Data();
                    logOutput << "Keepalive no longer found on d3d10.1, freeing capture data" << endl;
                    bCapturing = false;
                }

                keepAliveTime = timeVal;
            }
        }

        if(bHasTextures)
        {
            LONGLONG frameTime;
            if(bCapturing)
            {
                if(texData)
                {
                    if(frameTime = texData->frameTime)
                    {
                        LONGLONG timeElapsed = timeVal-lastTime;

                        if(timeElapsed >= frameTime)
                        {
                            if(!IsWindow(hwndOBS))
                            {
                                hwndOBS = NULL;
                                bStopRequested = true;
                            }

                            if(WaitForSingleObject(hSignalEnd, 0) == WAIT_OBJECT_0)
                                bStopRequested = true;

                            lastTime += frameTime;
                            if(timeElapsed > frameTime*2)
                                lastTime = timeVal;

                            DWORD nextCapture = curCapture == 0 ? 1 : 0;

                            ID3D10Resource *backBuffer = NULL;
                            if(SUCCEEDED(swap->GetBuffer(0, IID_ID3D10Resource, (void**)&backBuffer)))
                            {
                                if(bIsMultisampled)
                                    device->ResolveSubresource(copyD3D101TextureGame, 0, backBuffer, 0, dxgiFormat);
                                else
                                    device->CopyResource(copyD3D101TextureGame, backBuffer);

                                ID3D10Texture2D *outputTexture = NULL;
                                int lastRendered = -1;

                                if(keyedMutexes[curCapture]->AcquireSync(0, 0) == WAIT_OBJECT_0)
                                    lastRendered = (int)curCapture;
                                else if(keyedMutexes[nextCapture]->AcquireSync(0, 0) == WAIT_OBJECT_0)
                                    lastRendered = (int)nextCapture;

                                if(lastRendered != -1)
                                {
                                    shareDevice->CopyResource(sharedTextures[lastRendered], copyTextureIntermediary);
                                    keyedMutexes[lastRendered]->ReleaseSync(0);
                                }

                                texData->lastRendered = lastRendered;
                                backBuffer->Release();
                            }

                            curCapture = nextCapture;
                        }
                    }
                }
            }
            else
                ClearD3D101Data();
        }
    }

    device->Release();
}
