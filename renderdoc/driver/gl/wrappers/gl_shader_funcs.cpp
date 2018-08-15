/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2015-2018 Baldur Karlsson
 * Copyright (c) 2014 Crytek
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 ******************************************************************************/

#include "../gl_driver.h"
#include "../gl_shader_refl.h"
#include "common/common.h"
#include "driver/shaders/spirv/spirv_common.h"
#include "strings/string_utils.h"

enum GLshaderbitfield
{
};

DECLARE_REFLECTION_ENUM(GLshaderbitfield);

template <>
std::string DoStringise(const GLshaderbitfield &el)
{
  RDCCOMPILE_ASSERT(sizeof(GLshaderbitfield) == sizeof(GLbitfield) &&
                        sizeof(GLshaderbitfield) == sizeof(uint32_t),
                    "Fake bitfield enum must be uint32_t sized");

  BEGIN_BITFIELD_STRINGISE(GLshaderbitfield);
  {
    STRINGISE_BITFIELD_BIT(GL_VERTEX_SHADER_BIT);
    STRINGISE_BITFIELD_BIT(GL_TESS_CONTROL_SHADER_BIT);
    STRINGISE_BITFIELD_BIT(GL_TESS_EVALUATION_SHADER_BIT);
    STRINGISE_BITFIELD_BIT(GL_GEOMETRY_SHADER_BIT);
    STRINGISE_BITFIELD_BIT(GL_FRAGMENT_SHADER_BIT);
    STRINGISE_BITFIELD_BIT(GL_COMPUTE_SHADER_BIT);
  }
  END_BITFIELD_STRINGISE();
}

void WrappedOpenGL::ShaderData::ProcessSPIRVCompilation(WrappedOpenGL &drv, ResourceId id,
                                                        GLuint realShader, const GLchar *pEntryPoint,
                                                        GLuint numSpecializationConstants,
                                                        const GLuint *pConstantIndex,
                                                        const GLuint *pConstantValue)
{
  reflection.resourceId = id;
  reflection.entryPoint = pEntryPoint;
  reflection.stage = MakeShaderStage(type);
  reflection.encoding = ShaderEncoding::SPIRV;
  reflection.rawBytes.assign((byte *)spirv.spirv.data(), spirv.spirv.size() * sizeof(uint32_t));

  // we discard this too, because we don't need it - we don't do any SPIR-V patching in GL
  SPIRVPatchData patchData;

  spirv.MakeReflection(ShaderStage(ShaderIdx(type)), pEntryPoint, reflection, mapping, patchData);

  version = 460;

  entryPoint = pEntryPoint;
  if(numSpecializationConstants > 0)
  {
    specIDs.assign(pConstantIndex, pConstantIndex + numSpecializationConstants);
    specValues.assign(pConstantValue, pConstantValue + numSpecializationConstants);
  }

  GLuint sepshader = GL.glCreateShader(type);
  if(sepshader)
  {
    GL.glShaderBinary(1, &sepshader, eGL_SHADER_BINARY_FORMAT_SPIR_V, reflection.rawBytes.data(),
                      (GLsizei)reflection.rawBytes.size());

    GL.glSpecializeShader(sepshader, pEntryPoint, numSpecializationConstants, pConstantIndex,
                          pConstantValue);

    GLint compiled = 0;

    GL.glGetShaderiv(sepshader, eGL_COMPILE_STATUS, &compiled);

    if(compiled)
    {
      prog = GL.glCreateProgram();

      GL.glAttachShader(prog, sepshader);
      GL.glProgramParameteri(prog, eGL_PROGRAM_SEPARABLE, GL_TRUE);
      GL.glLinkProgram(prog);

      drv.glGetProgramiv(prog, eGL_LINK_STATUS, &compiled);

      if(!compiled)
      {
        RDCERR("Re-compiled but couldn't link SPIR-V program");
      }
    }
    else
    {
      RDCERR("Couldn't re-compile SPIR-V shader");
    }
    GL.glDeleteShader(sepshader);
  }
}

void WrappedOpenGL::ShaderData::ProcessCompilation(WrappedOpenGL &drv, ResourceId id,
                                                   GLuint realShader)
{
  bool pointSizeUsed = false, clipDistanceUsed = false;
  if(type == eGL_VERTEX_SHADER)
    CheckVertexOutputUses(sources, pointSizeUsed, clipDistanceUsed);

  entryPoint = "main";

  string concatenated;

  for(size_t i = 0; i < sources.size(); i++)
  {
    if(sources.size() > 1)
    {
      if(i > 0)
        concatenated += "\n";
      concatenated += "/////////////////////////////";
      concatenated += StringFormat::Fmt("// Source file %u", (uint32_t)i);
      concatenated += "/////////////////////////////";
      concatenated += "\n";
    }

    concatenated += sources[i];
  }

  size_t offs = concatenated.find("#version");

  if(offs == std::string::npos)
  {
    // if there's no #version it's assumed to be 100 which we set below
    version = 0;
  }
  else
  {
    // see if we find a second result after the first
    size_t offs2 = concatenated.find("#version", offs + 1);

    if(offs2 == std::string::npos)
    {
      version = ParseVersionStatement(concatenated.c_str() + offs);
    }
    else
    {
      // slow path, multiple #version matches so the first one might be in a comment. We need to
      // search from the start, past comments and whitespace, to find the first real #version.
      const char *search = concatenated.c_str();
      const char *end = search + concatenated.size();

      while(search < end)
      {
        // skip whitespace
        if(isspace(*search))
        {
          search++;
          continue;
        }

        // skip single-line C++ style comments
        if(search + 1 < end && search[0] == '/' && search[1] == '/')
        {
          // continue until the next newline
          while(search < end && search[0] != '\r' && search[0] != '\n')
            search++;

          // continue, the whitespace skip above will skip the newline
          continue;
        }

        // skip multi-line C style comments
        if(search + 1 < end && search[0] == '/' && search[1] == '*')
        {
          // continue until the ending marker
          while(search + 1 < end && (search[0] != '*' || search[1] != '/'))
            search++;

          // skip the end marker
          search += 2;

          // continue, the whitespace skip above will skip the newline
          continue;
        }

        // missing #version is valid, so just exit
        if(search + sizeof("#version") > end)
        {
          RDCERR("Bad shader - reached end of text after skipping all comments and whitespace");
          break;
        }

        std::string versionText(search, search + sizeof("#version") - 1);

        // if we found the version, parse it
        if(versionText == "#version")
          version = ParseVersionStatement(search);

        // otherwise break - a missing #version is valid, and a legal #version cannot occur anywhere
        // after this point.
        break;
      }
    }
  }

  // default to version 100
  if(version == 0)
    version = 100;

  reflection.encoding = ShaderEncoding::GLSL;
  reflection.rawBytes.assign((byte *)concatenated.c_str(), concatenated.size());

  GLuint sepProg = prog;

  GLint status = 0;
  if(realShader == 0)
    status = 1;
  else
    drv.glGetShaderiv(realShader, eGL_COMPILE_STATUS, &status);

  if(IsCaptureMode(drv.GetState()))
  {
    glslangShader = CompileShaderForReflection(SPIRVShaderStage(ShaderIdx(type)), sources);
  }
  else
  {
	vector<vector<string>> shaderSources(1, sources);
	for (string s : libSources)
	  shaderSources.push_back(vector<string>(1, s));

    if(sepProg == 0 && status == 1)
      sepProg = MakeSeparableShaderProgram(drv, type, shaderSources, NULL);

    if(status == 0)
    {
      RDCDEBUG("Real shader failed to compile, so skipping separable program and reflection.");
    }
    else if(sepProg == 0)
    {
      RDCERR(
          "Couldn't make separable program for shader via patching - functionality will be "
          "broken.");
    }
    else
    {
      prog = sepProg;
      MakeShaderReflection(type, sepProg, reflection, pointSizeUsed, clipDistanceUsed);

      vector<uint32_t> spirvwords;

      SPIRVCompilationSettings settings(SPIRVSourceLanguage::OpenGLGLSL,
                                        SPIRVShaderStage(ShaderIdx(type)));

      string s = CompileSPIRV(settings, sources, spirvwords);
      if(!spirvwords.empty())
        ParseSPIRV(&spirvwords.front(), spirvwords.size(), spirv);
      else
        disassembly = s;

      reflection.resourceId = id;
      reflection.entryPoint = "main";

      reflection.stage = MakeShaderStage(type);

      reflection.debugInfo.encoding = ShaderEncoding::GLSL;

      reflection.debugInfo.files.resize(1);
      reflection.debugInfo.files[0].filename = "main.glsl";
      reflection.debugInfo.files[0].contents = concatenated;
    }
  }
}

#pragma region Shaders

template <typename SerialiserType>
bool WrappedOpenGL::Serialise_glCreateShader(SerialiserType &ser, GLenum type, GLuint shader)
{
  SERIALISE_ELEMENT(type);
  SERIALISE_ELEMENT_LOCAL(Shader, GetResourceManager()->GetID(ShaderRes(GetCtx(), shader)))
      .TypedAs("GLResource");

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    GLuint real = GL.glCreateShader(type);

    GLResource res = ShaderRes(GetCtx(), real);

    ResourceId liveId = GetResourceManager()->RegisterResource(res);

    m_Shaders[liveId].type = type;

    GetResourceManager()->AddLiveResource(Shader, res);

    AddResource(Shader, ResourceType::Shader, "Shader");
  }

  return true;
}

GLuint WrappedOpenGL::glCreateShader(GLenum type)
{
  GLuint real;
  SERIALISE_TIME_CALL(real = GL.glCreateShader(type));

  GLResource res = ShaderRes(GetCtx(), real);
  ResourceId id = GetResourceManager()->RegisterResource(res);

  if(IsCaptureMode(m_State))
  {
    Chunk *chunk = NULL;

    {
      USE_SCRATCH_SERIALISER();
      SCOPED_SERIALISE_CHUNK(gl_CurChunk);
      Serialise_glCreateShader(ser, type, real);

      chunk = scope.Get();
    }

    GLResourceRecord *record = GetResourceManager()->AddResourceRecord(id);
    RDCASSERT(record);

    record->AddChunk(chunk);
  }
  else
  {
    GetResourceManager()->AddLiveResource(id, res);
  }

  m_Shaders[id].type = type;

  return real;
}

template <typename SerialiserType>
bool WrappedOpenGL::Serialise_glShaderSource(SerialiserType &ser, GLuint shaderHandle, GLsizei count,
                                             const GLchar *const *source, const GLint *length)
{
  SERIALISE_ELEMENT_LOCAL(shader, ShaderRes(GetCtx(), shaderHandle));

  // serialisation can't handle the length parameter neatly, so we compromise by serialising via a
  // vector
  std::vector<std::string> sources;

  if(ser.IsWriting())
  {
    sources.reserve(count);
    for(GLsizei c = 0; c < count; c++)
    {
      sources.push_back((length && length[c] > 0) ? std::string(source[c], source[c] + length[c])
                                                  : std::string(source[c]));
    }
  }

  SERIALISE_ELEMENT(count);
  SERIALISE_ELEMENT(sources);
  SERIALISE_ELEMENT_ARRAY(length, count);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    std::vector<const char *> strs;
    for(size_t i = 0; i < sources.size(); i++)
      strs.push_back(sources[i].c_str());

    ResourceId liveId = GetResourceManager()->GetID(shader);

    m_Shaders[liveId].sources = sources;

    GL.glShaderSource(shader.name, (GLsizei)sources.size(), strs.data(), NULL);

    // if we've already disassembled this shader, undo all that.
    // Note this means we don't support compiling the same shader multiple times
    // attached to different programs, but that is *utterly crazy* and anyone
    // who tries to actually do that should be ashamed.
    // Doing this means we support the case of recompiling a shader different ways
    // and relinking a program before use, which is still moderately crazy and
    // so people who do that should be moderately ashamed.
    if(m_Shaders[liveId].prog)
    {
      GL.glDeleteProgram(m_Shaders[liveId].prog);
      m_Shaders[liveId].prog = 0;
      m_Shaders[liveId].spirv = SPVModule();
      m_Shaders[liveId].reflection = ShaderReflection();
    }

    AddResourceInitChunk(shader);
  }

  return true;
}

void WrappedOpenGL::glShaderSource(GLuint shader, GLsizei count, const GLchar *const *string,
                                   const GLint *length)
{
  SERIALISE_TIME_CALL(GL.glShaderSource(shader, count, string, length));

  if(IsCaptureMode(m_State))
  {
    GLResourceRecord *record = GetResourceManager()->GetResourceRecord(ShaderRes(GetCtx(), shader));
    RDCASSERTMSG("Couldn't identify object passed to function. Mismatched or bad GLuint?", record,
                 shader);
    if(record)
    {
      USE_SCRATCH_SERIALISER();
      SCOPED_SERIALISE_CHUNK(gl_CurChunk);
      Serialise_glShaderSource(ser, shader, count, string, length);

      record->AddChunk(scope.Get());
    }
  }

  // if we're capturing and don't have ARB_program_interface_query we're going to have to emulate
  // it using glslang for compilation and reflection
  if(IsReplayMode(m_State) || !HasExt[ARB_program_interface_query])
  {
    ResourceId id = GetResourceManager()->GetID(ShaderRes(GetCtx(), shader));
    m_Shaders[id].sources.clear();
    m_Shaders[id].sources.reserve(count);

    for(GLsizei i = 0; i < count; i++)
      m_Shaders[id].sources.push_back(string[i]);
  }
}

template <typename SerialiserType>
bool WrappedOpenGL::Serialise_glCompileShader(SerialiserType &ser, GLuint shaderHandle)
{
  SERIALISE_ELEMENT_LOCAL(shader, ShaderRes(GetCtx(), shaderHandle));

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    ResourceId liveId = GetResourceManager()->GetID(shader);

    GL.glCompileShader(shader.name);

	// // defer ProcessCompilation to Serialise_glLinkProgram
    /*m_Shaders[liveId].ProcessCompilation(*this, GetResourceManager()->GetOriginalID(liveId),
                                         shader.name);*/

    AddResourceInitChunk(shader);
  }

  return true;
}

void WrappedOpenGL::glCompileShader(GLuint shader)
{
  GL.glCompileShader(shader);

  if(IsCaptureMode(m_State))
  {
    GLResourceRecord *record = GetResourceManager()->GetResourceRecord(ShaderRes(GetCtx(), shader));
    RDCASSERTMSG("Couldn't identify object passed to function. Mismatched or bad GLuint?", record,
                 shader);
    if(record)
    {
      USE_SCRATCH_SERIALISER();
      SCOPED_SERIALISE_CHUNK(gl_CurChunk);
      Serialise_glCompileShader(ser, shader);

      record->AddChunk(scope.Get());
    }
  }

  {
    ResourceId id = GetResourceManager()->GetID(ShaderRes(GetCtx(), shader));

    // if we're capturing and don't have ARB_program_interface_query we're going to have to emulate
    // it using glslang for compilation and reflection
    if(IsReplayMode(m_State) || !HasExt[ARB_program_interface_query])
      m_Shaders[id].ProcessCompilation(*this, id, shader);
  }
}

void WrappedOpenGL::glReleaseShaderCompiler()
{
  GL.glReleaseShaderCompiler();
}

void WrappedOpenGL::glDeleteShader(GLuint shader)
{
  GL.glDeleteShader(shader);

  GLResource res = ShaderRes(GetCtx(), shader);
  if(GetResourceManager()->HasCurrentResource(res))
  {
    if(GetResourceManager()->HasResourceRecord(res))
      GetResourceManager()->GetResourceRecord(res)->Delete(GetResourceManager());
    GetResourceManager()->UnregisterResource(res);
  }
}

template <typename SerialiserType>
bool WrappedOpenGL::Serialise_glAttachShader(SerialiserType &ser, GLuint programHandle,
                                             GLuint shaderHandle)
{
  SERIALISE_ELEMENT_LOCAL(program, ProgramRes(GetCtx(), programHandle));
  SERIALISE_ELEMENT_LOCAL(shader, ShaderRes(GetCtx(), shaderHandle));

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    ResourceId liveProgId = GetResourceManager()->GetID(program);
    ResourceId liveShadId = GetResourceManager()->GetID(shader);

    m_Programs[liveProgId].shaders.push_back(liveShadId);

    GL.glAttachShader(program.name, shader.name);

    AddResourceInitChunk(program);
    DerivedResource(program, GetResourceManager()->GetOriginalID(liveShadId));
  }

  return true;
}

void WrappedOpenGL::glAttachShader(GLuint program, GLuint shader)
{
  SERIALISE_TIME_CALL(GL.glAttachShader(program, shader));

  if(program && shader)
  {
    if(IsCaptureMode(m_State))
    {
      GLResourceRecord *progRecord =
          GetResourceManager()->GetResourceRecord(ProgramRes(GetCtx(), program));
      GLResourceRecord *shadRecord =
          GetResourceManager()->GetResourceRecord(ShaderRes(GetCtx(), shader));
      RDCASSERT(progRecord && shadRecord);
      if(progRecord && shadRecord)
      {
        USE_SCRATCH_SERIALISER();
        SCOPED_SERIALISE_CHUNK(gl_CurChunk);
        Serialise_glAttachShader(ser, program, shader);

        progRecord->AddParent(shadRecord);
        progRecord->AddChunk(scope.Get());
      }
    }

    // if we're capturing and don't have ARB_program_interface_query we're going to have to emulate
    // it using glslang for compilation and reflection
    if(IsReplayMode(m_State) || !HasExt[ARB_program_interface_query])
    {
      ResourceId progid = GetResourceManager()->GetID(ProgramRes(GetCtx(), program));
      ResourceId shadid = GetResourceManager()->GetID(ShaderRes(GetCtx(), shader));
      m_Programs[progid].shaders.push_back(shadid);
    }
  }
}

template <typename SerialiserType>
bool WrappedOpenGL::Serialise_glDetachShader(SerialiserType &ser, GLuint programHandle,
                                             GLuint shaderHandle)
{
  SERIALISE_ELEMENT_LOCAL(program, ProgramRes(GetCtx(), programHandle));
  SERIALISE_ELEMENT_LOCAL(shader, ShaderRes(GetCtx(), shaderHandle));

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    ResourceId liveProgId = GetResourceManager()->GetID(program);
    ResourceId liveShadId = GetResourceManager()->GetID(shader);

    // in order to be able to relink programs, we don't replay detaches. This should be valid as
    // it's legal to have a shader attached to multiple programs, so even if it's attached again
    // that doesn't affect the attach here.
    /*
    if(!m_Programs[liveProgId].linked)
    {
      for(auto it = m_Programs[liveProgId].shaders.begin();
          it != m_Programs[liveProgId].shaders.end(); ++it)
      {
        if(*it == liveShadId)
        {
          m_Programs[liveProgId].shaders.erase(it);
          break;
        }
      }
    }

    GL.glDetachShader(GetResourceManager()->GetLiveResource(progid).name,
                      GetResourceManager()->GetLiveResource(shadid).name);
    */
  }

  return true;
}

void WrappedOpenGL::glDetachShader(GLuint program, GLuint shader)
{
  SERIALISE_TIME_CALL(GL.glDetachShader(program, shader));

  if(program && shader)
  {
    // check that shader still exists, it might have been deleted. If it has, it's not too important
    // that we detach the shader (only important if the program will attach it elsewhere).
    if(IsCaptureMode(m_State) && GetResourceManager()->HasCurrentResource(ShaderRes(GetCtx(), shader)))
    {
      GLResourceRecord *progRecord =
          GetResourceManager()->GetResourceRecord(ProgramRes(GetCtx(), program));
      RDCASSERT(progRecord);
      {
        USE_SCRATCH_SERIALISER();
        SCOPED_SERIALISE_CHUNK(gl_CurChunk);
        Serialise_glDetachShader(ser, program, shader);

        progRecord->AddChunk(scope.Get());
      }
    }

    // if we're capturing and don't have ARB_program_interface_query we're going to have to emulate
    // it using glslang for compilation and reflection
    if(IsReplayMode(m_State) || !HasExt[ARB_program_interface_query])
    {
      ResourceId progid = GetResourceManager()->GetID(ProgramRes(GetCtx(), program));
      ResourceId shadid = GetResourceManager()->GetID(ShaderRes(GetCtx(), shader));

      if(!m_Programs[progid].linked)
      {
        for(auto it = m_Programs[progid].shaders.begin(); it != m_Programs[progid].shaders.end(); ++it)
        {
          if(*it == shadid)
          {
            m_Programs[progid].shaders.erase(it);
            break;
          }
        }
      }
    }
  }
}

#pragma endregion

#pragma region Programs

template <typename SerialiserType>
bool WrappedOpenGL::Serialise_glCreateShaderProgramv(SerialiserType &ser, GLenum type, GLsizei count,
                                                     const GLchar *const *strings, GLuint program)
{
  SERIALISE_ELEMENT(type);
  SERIALISE_ELEMENT(count);
  SERIALISE_ELEMENT_ARRAY(strings, count);
  SERIALISE_ELEMENT_LOCAL(Program, GetResourceManager()->GetID(ProgramRes(GetCtx(), program)))
      .TypedAs("GLResource");

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    std::vector<std::string> src;
    for(GLsizei i = 0; i < count; i++)
      src.push_back(strings[i]);

    GLuint real = GL.glCreateShaderProgramv(type, count, strings);
    // we want a separate program that we can mess about with for making overlays
    // and relink without having to worry about restoring the 'real' program state.
    GLuint sepprog = MakeSeparableShaderProgram(*this, type, src, NULL);

    GLResource res = ProgramRes(GetCtx(), real);

    ResourceId liveId = m_ResourceManager->RegisterResource(res);

    auto &progDetails = m_Programs[liveId];

    progDetails.linked = true;
    progDetails.shaders.push_back(liveId);
    progDetails.stageShaders[ShaderIdx(type)] = liveId;
    progDetails.shaderProgramUnlinkable = true;

    auto &shadDetails = m_Shaders[liveId];

    shadDetails.type = type;
    shadDetails.sources.swap(src);
    shadDetails.prog = sepprog;

    shadDetails.ProcessCompilation(*this, Program, 0);

    GetResourceManager()->AddLiveResource(Program, res);

    AddResource(Program, ResourceType::StateObject, "Program");
  }

  return true;
}

GLuint WrappedOpenGL::glCreateShaderProgramv(GLenum type, GLsizei count, const GLchar *const *strings)
{
  GLuint real;
  SERIALISE_TIME_CALL(real = GL.glCreateShaderProgramv(type, count, strings));

  if(real == 0)
    return real;

  GLResource res = ProgramRes(GetCtx(), real);
  ResourceId id = GetResourceManager()->RegisterResource(res);

  if(IsCaptureMode(m_State))
  {
    Chunk *chunk = NULL;

    {
      USE_SCRATCH_SERIALISER();
      SCOPED_SERIALISE_CHUNK(gl_CurChunk);
      Serialise_glCreateShaderProgramv(ser, type, count, strings, real);

      chunk = scope.Get();
    }

    GLResourceRecord *record = GetResourceManager()->AddResourceRecord(id);
    RDCASSERT(record);

    // we always want to mark programs as dirty so we can serialise their
    // locations as initial state (and form a remapping table)
    GetResourceManager()->MarkDirtyResource(id);

    record->AddChunk(chunk);
  }
  else
  {
    GetResourceManager()->AddLiveResource(id, res);

    vector<string> src;
    for(GLsizei i = 0; i < count; i++)
      src.push_back(strings[i]);

    GLuint sepprog = MakeSeparableShaderProgram(*this, type, src, NULL);

    auto &progDetails = m_Programs[id];

    progDetails.linked = true;
    progDetails.shaders.push_back(id);
    progDetails.stageShaders[ShaderIdx(type)] = id;

    auto &shadDetails = m_Shaders[id];

    shadDetails.type = type;
    shadDetails.sources.swap(src);
    shadDetails.prog = sepprog;

    shadDetails.ProcessCompilation(*this, id, 0);
  }

  return real;
}

template <typename SerialiserType>
bool WrappedOpenGL::Serialise_glCreateProgram(SerialiserType &ser, GLuint program)
{
  SERIALISE_ELEMENT_LOCAL(Program, GetResourceManager()->GetID(ProgramRes(GetCtx(), program)))
      .TypedAs("GLResource");

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    GLuint real = GL.glCreateProgram();

    GLResource res = ProgramRes(GetCtx(), real);

    ResourceId liveId = m_ResourceManager->RegisterResource(res);

    m_Programs[liveId].linked = false;

    GetResourceManager()->AddLiveResource(Program, res);

    AddResource(Program, ResourceType::StateObject, "Program");
  }

  return true;
}

GLuint WrappedOpenGL::glCreateProgram()
{
  GLuint real;
  SERIALISE_TIME_CALL(real = GL.glCreateProgram());

  GLResource res = ProgramRes(GetCtx(), real);
  ResourceId id = GetResourceManager()->RegisterResource(res);

  if(IsCaptureMode(m_State))
  {
    Chunk *chunk = NULL;

    {
      USE_SCRATCH_SERIALISER();
      SCOPED_SERIALISE_CHUNK(gl_CurChunk);
      Serialise_glCreateProgram(ser, real);

      chunk = scope.Get();
    }

    GLResourceRecord *record = GetResourceManager()->AddResourceRecord(id);
    RDCASSERT(record);

    // we always want to mark programs as dirty so we can serialise their
    // locations as initial state (and form a remapping table)
    GetResourceManager()->MarkDirtyResource(id);

    record->AddChunk(chunk);
  }
  else
  {
    GetResourceManager()->AddLiveResource(id, res);
  }

  m_Programs[id].linked = false;

  return real;
}

template <typename SerialiserType>
bool WrappedOpenGL::Serialise_glLinkProgram(SerialiserType &ser, GLuint programHandle)
{
  SERIALISE_ELEMENT_LOCAL(program, ProgramRes(GetCtx(), programHandle));

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    ResourceId progid = GetResourceManager()->GetID(program);

    ProgramData &progDetails = m_Programs[progid];

    progDetails.linked = true;

    for(size_t s = 0; s < 6; s++)
    {
	  // get all stage shaders
	  std::vector<ResourceId> stageShaders;
      for(size_t sh = 0; sh < progDetails.shaders.size(); sh++)
      {
		if(m_Shaders[progDetails.shaders[sh]].type == ShaderEnum(s))
		  stageShaders.push_back(progDetails.shaders[sh]);
      }

	  ResourceId mainId;

	  if (stageShaders.size() > 1) {
		  // find shader with main, there should be only and only one per stage
		  for (auto &shad : stageShaders) {
			  // TODO handle better main() search
			  if (m_Shaders[shad].sources[0].find("main") != std::string::npos) {
				  mainId = shad;
			  }
		  }

		  ShaderData &main = m_Shaders[mainId];

		  // TODO compile shaders separately instead of deleting #version and concatenating
		  // combine sources in order to enable reflection
		  for (auto &shad : stageShaders) {
			  // there can be only one main
			  if (shad != mainId) {
				  for (std::string &src : m_Shaders[shad].sources) {
					  std::string append(src);
					  // TODO handle better #version search, extract from ShaderData::ProcessCompilation?
					  /*size_t off = src.find("#version");
					  if (off != std::string::npos) {
						  size_t end = off + 8;
						  // skip spaces
						  while (isspace(src.at(end))) {
							  end++;
						  }
						  // skip digits
						  while (isdigit(src.at(end))) {
							  end++;
						  }
						  std::string ver = append.substr(off, end - off);
						  append.replace(off, end - off, "// " + ver); // comment #version ddd
					  }*/
					  main.libSources.push_back(append);
				  }
			  }
		  }
	  }
	  else if (stageShaders.size() == 1)
	  {
		  mainId = stageShaders[0];
	  }

	  if (mainId != ResourceId()) {
		  progDetails.stageShaders[s] = mainId;

		  GLResource shader = GetResourceManager()->GetCurrentResource(mainId);

		  m_Shaders[mainId].ProcessCompilation(*this, mainId, shader.name);
	  }
    }

    GL.glLinkProgram(program.name);

    AddResourceInitChunk(program);
  }

  return true;
}

void WrappedOpenGL::glLinkProgram(GLuint program)
{
  SERIALISE_TIME_CALL(GL.glLinkProgram(program));

  if(IsCaptureMode(m_State))
  {
    GLResourceRecord *record = GetResourceManager()->GetResourceRecord(ProgramRes(GetCtx(), program));
    RDCASSERTMSG("Couldn't identify object passed to function. Mismatched or bad GLuint?", record,
                 program);
    if(record)
    {
      USE_SCRATCH_SERIALISER();
      SCOPED_SERIALISE_CHUNK(gl_CurChunk);
      Serialise_glLinkProgram(ser, program);

      record->AddChunk(scope.Get());
    }
  }

  {
    ResourceId progid = GetResourceManager()->GetID(ProgramRes(GetCtx(), program));

    ProgramData &progDetails = m_Programs[progid];

    progDetails.linked = true;

    for(size_t s = 0; s < 6; s++)
    {
      for(size_t sh = 0; sh < progDetails.shaders.size(); sh++)
      {
        if(m_Shaders[progDetails.shaders[sh]].type == ShaderEnum(s))
          progDetails.stageShaders[s] = progDetails.shaders[sh];
      }
    }

    if(IsCaptureMode(m_State) && !HasExt[ARB_program_interface_query])
    {
      std::vector<glslang::TShader *> glslangShaders;

      for(ResourceId id : progDetails.shaders)
      {
        glslang::TShader *s = m_Shaders[id].glslangShader;
        if(s == NULL)
        {
          RDCERR("Shader attached with no compiled glslang reflection shader!");
          continue;
        }

        glslangShaders.push_back(m_Shaders[id].glslangShader);
      }

      progDetails.glslangProgram = LinkProgramForReflection(glslangShaders);
    }
  }
}

template <typename SerialiserType>
bool WrappedOpenGL::Serialise_glUniformBlockBinding(SerialiserType &ser, GLuint programHandle,
                                                    GLuint uniformBlockIndex,
                                                    GLuint uniformBlockBinding)
{
  SERIALISE_ELEMENT_LOCAL(program, ProgramRes(GetCtx(), programHandle));
  SERIALISE_ELEMENT(uniformBlockIndex);
  SERIALISE_ELEMENT(uniformBlockBinding);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    GL.glUniformBlockBinding(program.name, uniformBlockIndex, uniformBlockBinding);
  }

  return true;
}

void WrappedOpenGL::glUniformBlockBinding(GLuint program, GLuint uniformBlockIndex,
                                          GLuint uniformBlockBinding)
{
  SERIALISE_TIME_CALL(GL.glUniformBlockBinding(program, uniformBlockIndex, uniformBlockBinding));

  if(IsCaptureMode(m_State))
  {
    GLResourceRecord *record = GetResourceManager()->GetResourceRecord(ProgramRes(GetCtx(), program));
    RDCASSERTMSG("Couldn't identify object passed to function. Mismatched or bad GLuint?", record,
                 program);
    if(record)
    {
      USE_SCRATCH_SERIALISER();
      SCOPED_SERIALISE_CHUNK(gl_CurChunk);
      Serialise_glUniformBlockBinding(ser, program, uniformBlockIndex, uniformBlockBinding);

      record->AddChunk(scope.Get());
    }
  }
}

template <typename SerialiserType>
bool WrappedOpenGL::Serialise_glShaderStorageBlockBinding(SerialiserType &ser, GLuint programHandle,
                                                          GLuint storageBlockIndex,
                                                          GLuint storageBlockBinding)
{
  SERIALISE_ELEMENT_LOCAL(program, ProgramRes(GetCtx(), programHandle));
  SERIALISE_ELEMENT(storageBlockIndex);
  SERIALISE_ELEMENT(storageBlockBinding);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    GL.glShaderStorageBlockBinding(program.name, storageBlockIndex, storageBlockBinding);
  }

  return true;
}

void WrappedOpenGL::glShaderStorageBlockBinding(GLuint program, GLuint storageBlockIndex,
                                                GLuint storageBlockBinding)
{
  SERIALISE_TIME_CALL(GL.glShaderStorageBlockBinding(program, storageBlockIndex, storageBlockBinding));

  if(IsCaptureMode(m_State))
  {
    GLResourceRecord *record = GetResourceManager()->GetResourceRecord(ProgramRes(GetCtx(), program));
    RDCASSERTMSG("Couldn't identify object passed to function. Mismatched or bad GLuint?", record,
                 program);
    if(record)
    {
      USE_SCRATCH_SERIALISER();
      SCOPED_SERIALISE_CHUNK(gl_CurChunk);
      Serialise_glShaderStorageBlockBinding(ser, program, storageBlockIndex, storageBlockBinding);

      record->AddChunk(scope.Get());
    }
  }
}

template <typename SerialiserType>
bool WrappedOpenGL::Serialise_glBindAttribLocation(SerialiserType &ser, GLuint programHandle,
                                                   GLuint index, const GLchar *name)
{
  SERIALISE_ELEMENT_LOCAL(program, ProgramRes(GetCtx(), programHandle));
  SERIALISE_ELEMENT(index);
  SERIALISE_ELEMENT(name);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    GL.glBindAttribLocation(program.name, index, name);
  }

  return true;
}

void WrappedOpenGL::glBindAttribLocation(GLuint program, GLuint index, const GLchar *name)
{
  SERIALISE_TIME_CALL(GL.glBindAttribLocation(program, index, name));

  if(IsCaptureMode(m_State))
  {
    GLResourceRecord *record = GetResourceManager()->GetResourceRecord(ProgramRes(GetCtx(), program));
    RDCASSERTMSG("Couldn't identify object passed to function. Mismatched or bad GLuint?", record,
                 program);
    if(record)
    {
      USE_SCRATCH_SERIALISER();
      SCOPED_SERIALISE_CHUNK(gl_CurChunk);
      Serialise_glBindAttribLocation(ser, program, index, name);

      record->AddChunk(scope.Get());
    }
  }
}

template <typename SerialiserType>
bool WrappedOpenGL::Serialise_glBindFragDataLocation(SerialiserType &ser, GLuint programHandle,
                                                     GLuint color, const GLchar *name)
{
  SERIALISE_ELEMENT_LOCAL(program, ProgramRes(GetCtx(), programHandle));
  SERIALISE_ELEMENT(color);
  SERIALISE_ELEMENT(name);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    GL.glBindFragDataLocation(program.name, color, name);
  }

  return true;
}

void WrappedOpenGL::glBindFragDataLocation(GLuint program, GLuint color, const GLchar *name)
{
  SERIALISE_TIME_CALL(GL.glBindFragDataLocation(program, color, name));

  if(IsCaptureMode(m_State))
  {
    GLResourceRecord *record = GetResourceManager()->GetResourceRecord(ProgramRes(GetCtx(), program));
    RDCASSERTMSG("Couldn't identify object passed to function. Mismatched or bad GLuint?", record,
                 program);
    if(record)
    {
      USE_SCRATCH_SERIALISER();
      SCOPED_SERIALISE_CHUNK(gl_CurChunk);
      Serialise_glBindFragDataLocation(ser, program, color, name);

      record->AddChunk(scope.Get());
    }
  }
}

template <typename SerialiserType>
bool WrappedOpenGL::Serialise_glUniformSubroutinesuiv(SerialiserType &ser, GLenum shadertype,
                                                      GLsizei count, const GLuint *indices)
{
  SERIALISE_ELEMENT(shadertype);
  SERIALISE_ELEMENT(count);
  SERIALISE_ELEMENT_ARRAY(indices, count);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    GL.glUniformSubroutinesuiv(shadertype, count, indices);

    APIProps.ShaderLinkage = true;
  }

  return true;
}

void WrappedOpenGL::glUniformSubroutinesuiv(GLenum shadertype, GLsizei count, const GLuint *indices)
{
  SERIALISE_TIME_CALL(GL.glUniformSubroutinesuiv(shadertype, count, indices));

  if(IsActiveCapturing(m_State))
  {
    USE_SCRATCH_SERIALISER();
    SCOPED_SERIALISE_CHUNK(gl_CurChunk);
    Serialise_glUniformSubroutinesuiv(ser, shadertype, count, indices);

    GetContextRecord()->AddChunk(scope.Get());
  }
}

template <typename SerialiserType>
bool WrappedOpenGL::Serialise_glBindFragDataLocationIndexed(SerialiserType &ser,
                                                            GLuint programHandle, GLuint colorNumber,
                                                            GLuint index, const GLchar *name)
{
  SERIALISE_ELEMENT_LOCAL(program, ProgramRes(GetCtx(), programHandle));
  SERIALISE_ELEMENT(colorNumber);
  SERIALISE_ELEMENT(index);
  SERIALISE_ELEMENT(name);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    GL.glBindFragDataLocationIndexed(program.name, colorNumber, index, name);
  }

  return true;
}

void WrappedOpenGL::glBindFragDataLocationIndexed(GLuint program, GLuint colorNumber, GLuint index,
                                                  const GLchar *name)
{
  SERIALISE_TIME_CALL(GL.glBindFragDataLocationIndexed(program, colorNumber, index, name));

  if(IsCaptureMode(m_State))
  {
    GLResourceRecord *record = GetResourceManager()->GetResourceRecord(ProgramRes(GetCtx(), program));
    RDCASSERTMSG("Couldn't identify object passed to function. Mismatched or bad GLuint?", record,
                 program);
    if(record)
    {
      USE_SCRATCH_SERIALISER();
      SCOPED_SERIALISE_CHUNK(gl_CurChunk);
      Serialise_glBindFragDataLocationIndexed(ser, program, colorNumber, index, name);

      record->AddChunk(scope.Get());
    }
  }
}

template <typename SerialiserType>
bool WrappedOpenGL::Serialise_glTransformFeedbackVaryings(SerialiserType &ser, GLuint programHandle,
                                                          GLsizei count,
                                                          const GLchar *const *varyings,
                                                          GLenum bufferMode)
{
  SERIALISE_ELEMENT_LOCAL(program, ProgramRes(GetCtx(), programHandle));
  SERIALISE_ELEMENT(count);
  SERIALISE_ELEMENT_ARRAY(varyings, count);
  SERIALISE_ELEMENT(bufferMode);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    GL.glTransformFeedbackVaryings(program.name, count, varyings, bufferMode);
  }

  return true;
}

void WrappedOpenGL::glTransformFeedbackVaryings(GLuint program, GLsizei count,
                                                const GLchar *const *varyings, GLenum bufferMode)
{
  SERIALISE_TIME_CALL(GL.glTransformFeedbackVaryings(program, count, varyings, bufferMode));

  if(IsCaptureMode(m_State))
  {
    GLResourceRecord *record = GetResourceManager()->GetResourceRecord(ProgramRes(GetCtx(), program));
    RDCASSERTMSG("Couldn't identify object passed to function. Mismatched or bad GLuint?", record,
                 program);
    if(record)
    {
      USE_SCRATCH_SERIALISER();
      SCOPED_SERIALISE_CHUNK(gl_CurChunk);
      Serialise_glTransformFeedbackVaryings(ser, program, count, varyings, bufferMode);

      record->AddChunk(scope.Get());
    }
  }
}

template <typename SerialiserType>
bool WrappedOpenGL::Serialise_glProgramParameteri(SerialiserType &ser, GLuint programHandle,
                                                  GLenum pname, GLint value)
{
  SERIALISE_ELEMENT_LOCAL(program, ProgramRes(GetCtx(), programHandle));
  SERIALISE_ELEMENT(pname);
  SERIALISE_ELEMENT(value);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    GL.glProgramParameteri(program.name, pname, value);
  }

  return true;
}

void WrappedOpenGL::glProgramParameteri(GLuint program, GLenum pname, GLint value)
{
  SERIALISE_TIME_CALL(GL.glProgramParameteri(program, pname, value));

  if(IsCaptureMode(m_State))
  {
    GLResourceRecord *record = GetResourceManager()->GetResourceRecord(ProgramRes(GetCtx(), program));
    RDCASSERTMSG("Couldn't identify object passed to function. Mismatched or bad GLuint?", record,
                 program);
    if(record)
    {
      USE_SCRATCH_SERIALISER();
      SCOPED_SERIALISE_CHUNK(gl_CurChunk);
      Serialise_glProgramParameteri(ser, program, pname, value);

      record->AddChunk(scope.Get());
    }
  }
}

void WrappedOpenGL::glDeleteProgram(GLuint program)
{
  GL.glDeleteProgram(program);

  GLResource res = ProgramRes(GetCtx(), program);
  if(GetResourceManager()->HasCurrentResource(res))
  {
    GetResourceManager()->MarkCleanResource(res);
    if(GetResourceManager()->HasResourceRecord(res))
      GetResourceManager()->GetResourceRecord(res)->Delete(GetResourceManager());
    GetResourceManager()->UnregisterResource(res);
  }
}

template <typename SerialiserType>
bool WrappedOpenGL::Serialise_glUseProgram(SerialiserType &ser, GLuint programHandle)
{
  SERIALISE_ELEMENT_LOCAL(program, ProgramRes(GetCtx(), programHandle));

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    GL.glUseProgram(program.name);
  }

  return true;
}

void WrappedOpenGL::glUseProgram(GLuint program)
{
  SERIALISE_TIME_CALL(GL.glUseProgram(program));

  GetCtxData().m_Program = program;

  if(IsActiveCapturing(m_State))
  {
    USE_SCRATCH_SERIALISER();
    SCOPED_SERIALISE_CHUNK(gl_CurChunk);
    Serialise_glUseProgram(ser, program);

    GetContextRecord()->AddChunk(scope.Get());
    GetResourceManager()->MarkResourceFrameReferenced(ProgramRes(GetCtx(), program), eFrameRef_Read);
  }
}

void WrappedOpenGL::glValidateProgram(GLuint program)
{
  GL.glValidateProgram(program);
}

void WrappedOpenGL::glValidateProgramPipeline(GLuint pipeline)
{
  GL.glValidateProgramPipeline(pipeline);
}

template <typename SerialiserType>
bool WrappedOpenGL::Serialise_glShaderBinary(SerialiserType &ser, GLsizei count,
                                             const GLuint *shaders, GLenum binaryformat,
                                             const void *binary, GLsizei length)
{
  SERIALISE_ELEMENT(count);
  SERIALISE_ELEMENT_LOCAL(shader, ShaderRes(GetCtx(), shaders[0]));
  SERIALISE_ELEMENT(binaryformat);
  SERIALISE_ELEMENT_ARRAY(binary, length);
  SERIALISE_ELEMENT(length);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    ResourceId liveId = GetResourceManager()->GetID(shader);

    GL.glShaderBinary(1, &shader.name, binaryformat, binary, length);

    m_Shaders[liveId].spirvWords.assign((uint32_t *)binary, (uint32_t *)((byte *)binary + length));

    AddResourceInitChunk(shader);
  }

  return true;
}

void WrappedOpenGL::glShaderBinary(GLsizei count, const GLuint *shaders, GLenum binaryformat,
                                   const void *binary, GLsizei length)
{
  // conditionally forward on this call when capturing, since we want to coax the app into
  // providing non-binary shaders unless it's a format we understand: SPIR-V.
  if(IsReplayMode(m_State))
  {
    GL.glShaderBinary(count, shaders, binaryformat, binary, length);
  }
  else if(IsCaptureMode(m_State) && binaryformat == eGL_SHADER_BINARY_FORMAT_SPIR_V)
  {
    SERIALISE_TIME_CALL(GL.glShaderBinary(count, shaders, binaryformat, binary, length));

    for(GLsizei i = 0; i < count; i++)
    {
      GLResourceRecord *record =
          GetResourceManager()->GetResourceRecord(ShaderRes(GetCtx(), shaders[i]));
      RDCASSERTMSG("Couldn't identify object passed to function. Mismatched or bad GLuint?", record,
                   shaders[i]);
      if(record)
      {
        USE_SCRATCH_SERIALISER();
        SCOPED_SERIALISE_CHUNK(gl_CurChunk);
        Serialise_glShaderBinary(ser, 1, shaders + i, binaryformat, binary, length);

        record->AddChunk(scope.Get());
      }
    }
  }
}

void WrappedOpenGL::glProgramBinary(GLuint program, GLenum binaryFormat, const void *binary,
                                    GLsizei length)
{
  // deliberately don't forward on this call when writing, since we want to coax the app into
  // providing non-binary shaders.
  if(IsReplayMode(m_State))
  {
    GL.glProgramBinary(program, binaryFormat, binary, length);
  }
}

#pragma endregion

#pragma region Program Pipelines

template <typename SerialiserType>
bool WrappedOpenGL::Serialise_glUseProgramStages(SerialiserType &ser, GLuint pipelineHandle,
                                                 GLbitfield stages, GLuint programHandle)
{
  SERIALISE_ELEMENT_LOCAL(pipeline, ProgramPipeRes(GetCtx(), pipelineHandle));
  SERIALISE_ELEMENT_TYPED(GLshaderbitfield, stages);
  SERIALISE_ELEMENT_LOCAL(program, ProgramRes(GetCtx(), programHandle));

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    if(program.name)
    {
      ResourceId livePipeId = GetResourceManager()->GetID(pipeline);
      ResourceId liveProgId = GetResourceManager()->GetID(program);

      PipelineData &pipeDetails = m_Pipelines[livePipeId];
      ProgramData &progDetails = m_Programs[liveProgId];

      for(size_t s = 0; s < 6; s++)
      {
        if(stages & ShaderBit(s))
        {
          for(size_t sh = 0; sh < progDetails.shaders.size(); sh++)
          {
            if(m_Shaders[progDetails.shaders[sh]].type == ShaderEnum(s))
            {
              pipeDetails.stagePrograms[s] = liveProgId;
              pipeDetails.stageShaders[s] = progDetails.shaders[sh];
              break;
            }
          }
        }
      }

      GL.glUseProgramStages(pipeline.name, stages, program.name);
    }
    else
    {
      ResourceId livePipeId = GetResourceManager()->GetID(pipeline);
      PipelineData &pipeDetails = m_Pipelines[livePipeId];

      for(size_t s = 0; s < 6; s++)
      {
        if(stages & ShaderBit(s))
        {
          pipeDetails.stagePrograms[s] = ResourceId();
          pipeDetails.stageShaders[s] = ResourceId();
        }
      }

      GL.glUseProgramStages(pipeline.name, stages, 0);
    }
  }

  return true;
}

void WrappedOpenGL::glUseProgramStages(GLuint pipeline, GLbitfield stages, GLuint program)
{
  SERIALISE_TIME_CALL(GL.glUseProgramStages(pipeline, stages, program));

  if(IsCaptureMode(m_State))
  {
    GLResourceRecord *record =
        GetResourceManager()->GetResourceRecord(ProgramPipeRes(GetCtx(), pipeline));

    RDCASSERTMSG("Couldn't identify object passed to function. Mismatched or bad GLuint?", record,
                 pipeline);

    if(record == NULL)
      return;

    if(program)
    {
      GLResourceRecord *progrecord =
          GetResourceManager()->GetResourceRecord(ProgramRes(GetCtx(), program));
      RDCASSERT(progrecord);

      if(progrecord)
        record->AddParent(progrecord);
    }

    if(m_HighTrafficResources.find(record->GetResourceID()) != m_HighTrafficResources.end() &&
       IsBackgroundCapturing(m_State))
      return;

    USE_SCRATCH_SERIALISER();
    SCOPED_SERIALISE_CHUNK(gl_CurChunk);
    Serialise_glUseProgramStages(ser, pipeline, stages, program);

    Chunk *chunk = scope.Get();

    if(IsActiveCapturing(m_State))
    {
      GetContextRecord()->AddChunk(chunk);
    }
    else
    {
      record->AddChunk(chunk);
      record->UpdateCount++;

      if(record->UpdateCount > 10)
      {
        m_HighTrafficResources.insert(record->GetResourceID());
        GetResourceManager()->MarkDirtyResource(record->GetResourceID());
      }
    }
  }
  else
  {
    if(program)
    {
      ResourceId pipeID = GetResourceManager()->GetID(ProgramPipeRes(GetCtx(), pipeline));
      ResourceId progID = GetResourceManager()->GetID(ProgramRes(GetCtx(), program));

      PipelineData &pipeDetails = m_Pipelines[pipeID];
      ProgramData &progDetails = m_Programs[progID];

      for(size_t s = 0; s < 6; s++)
      {
        if(stages & ShaderBit(s))
        {
          for(size_t sh = 0; sh < progDetails.shaders.size(); sh++)
          {
            if(m_Shaders[progDetails.shaders[sh]].type == ShaderEnum(s))
            {
              pipeDetails.stagePrograms[s] = progID;
              pipeDetails.stageShaders[s] = progDetails.shaders[sh];
              break;
            }
          }
        }
      }
    }
    else
    {
      ResourceId pipeID = GetResourceManager()->GetID(ProgramPipeRes(GetCtx(), pipeline));
      PipelineData &pipeDetails = m_Pipelines[pipeID];

      for(size_t s = 0; s < 6; s++)
      {
        if(stages & ShaderBit(s))
        {
          pipeDetails.stagePrograms[s] = ResourceId();
          pipeDetails.stageShaders[s] = ResourceId();
        }
      }
    }
  }
}

template <typename SerialiserType>
bool WrappedOpenGL::Serialise_glGenProgramPipelines(SerialiserType &ser, GLsizei n, GLuint *pipelines)
{
  SERIALISE_ELEMENT(n);
  SERIALISE_ELEMENT_LOCAL(pipeline, GetResourceManager()->GetID(ProgramPipeRes(GetCtx(), *pipelines)))
      .TypedAs("GLResource");

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    GLuint real = 0;
    GL.glGenProgramPipelines(1, &real);
    GL.glBindProgramPipeline(real);
    GL.glBindProgramPipeline(0);

    GLResource res = ProgramPipeRes(GetCtx(), real);

    ResourceId live = m_ResourceManager->RegisterResource(res);
    GetResourceManager()->AddLiveResource(pipeline, res);

    AddResource(pipeline, ResourceType::StateObject, "Pipeline");
  }

  return true;
}

void WrappedOpenGL::glGenProgramPipelines(GLsizei n, GLuint *pipelines)
{
  SERIALISE_TIME_CALL(GL.glGenProgramPipelines(n, pipelines));

  for(GLsizei i = 0; i < n; i++)
  {
    GLResource res = ProgramPipeRes(GetCtx(), pipelines[i]);
    ResourceId id = GetResourceManager()->RegisterResource(res);

    if(IsCaptureMode(m_State))
    {
      Chunk *chunk = NULL;

      {
        USE_SCRATCH_SERIALISER();
        SCOPED_SERIALISE_CHUNK(gl_CurChunk);
        Serialise_glGenProgramPipelines(ser, 1, pipelines + i);

        chunk = scope.Get();
      }

      GLResourceRecord *record = GetResourceManager()->AddResourceRecord(id);
      RDCASSERT(record);

      record->AddChunk(chunk);
    }
    else
    {
      GetResourceManager()->AddLiveResource(id, res);
    }
  }
}

template <typename SerialiserType>
bool WrappedOpenGL::Serialise_glCreateProgramPipelines(SerialiserType &ser, GLsizei n,
                                                       GLuint *pipelines)
{
  SERIALISE_ELEMENT(n);
  SERIALISE_ELEMENT_LOCAL(pipeline, GetResourceManager()->GetID(ProgramPipeRes(GetCtx(), *pipelines)))
      .TypedAs("GLResource");

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    GLuint real = 0;
    GL.glCreateProgramPipelines(1, &real);

    GLResource res = ProgramPipeRes(GetCtx(), real);

    ResourceId live = m_ResourceManager->RegisterResource(res);
    GetResourceManager()->AddLiveResource(pipeline, res);

    AddResource(pipeline, ResourceType::StateObject, "Pipeline");
  }

  return true;
}

void WrappedOpenGL::glCreateProgramPipelines(GLsizei n, GLuint *pipelines)
{
  SERIALISE_TIME_CALL(GL.glCreateProgramPipelines(n, pipelines));

  for(GLsizei i = 0; i < n; i++)
  {
    GLResource res = ProgramPipeRes(GetCtx(), pipelines[i]);
    ResourceId id = GetResourceManager()->RegisterResource(res);

    if(IsCaptureMode(m_State))
    {
      Chunk *chunk = NULL;

      {
        USE_SCRATCH_SERIALISER();
        SCOPED_SERIALISE_CHUNK(gl_CurChunk);
        Serialise_glCreateProgramPipelines(ser, 1, pipelines + i);

        chunk = scope.Get();
      }

      GLResourceRecord *record = GetResourceManager()->AddResourceRecord(id);
      RDCASSERT(record);

      record->AddChunk(chunk);
    }
    else
    {
      GetResourceManager()->AddLiveResource(id, res);
    }
  }
}

template <typename SerialiserType>
bool WrappedOpenGL::Serialise_glBindProgramPipeline(SerialiserType &ser, GLuint pipelineHandle)
{
  SERIALISE_ELEMENT_LOCAL(pipeline, ProgramPipeRes(GetCtx(), pipelineHandle));

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    GL.glBindProgramPipeline(pipeline.name);
  }

  return true;
}

void WrappedOpenGL::glBindProgramPipeline(GLuint pipeline)
{
  SERIALISE_TIME_CALL(GL.glBindProgramPipeline(pipeline));

  GetCtxData().m_ProgramPipeline = pipeline;

  if(IsActiveCapturing(m_State))
  {
    USE_SCRATCH_SERIALISER();
    SCOPED_SERIALISE_CHUNK(gl_CurChunk);
    Serialise_glBindProgramPipeline(ser, pipeline);

    GetContextRecord()->AddChunk(scope.Get());
    GetResourceManager()->MarkResourceFrameReferenced(ProgramPipeRes(GetCtx(), pipeline),
                                                      eFrameRef_Read);
  }
}

void WrappedOpenGL::glActiveShaderProgram(GLuint pipeline, GLuint program)
{
  GL.glActiveShaderProgram(pipeline, program);
}

GLuint WrappedOpenGL::GetUniformProgram()
{
  ContextData &cd = GetCtxData();

  // program gets first dibs, if one is bound then that's where glUniform* calls go.
  if(cd.m_Program != 0)
  {
    return cd.m_Program;
  }
  else if(cd.m_ProgramPipeline != 0)
  {
    GLuint ret = 0;

    // otherwise, query the active program for the pipeline (could cache this above in
    // glActiveShaderProgram)
    // we do this query every time instead of caching the result, since I think it's unlikely that
    // we'll ever hit this path (most people using separable programs will use the glProgramUniform*
    // interface).
    // That way we don't pay the cost of a potentially expensive query unless we really need it.
    GL.glGetProgramPipelineiv(cd.m_ProgramPipeline, eGL_ACTIVE_PROGRAM, (GLint *)&ret);

    return ret;
  }

  return 0;
}

void WrappedOpenGL::glDeleteProgramPipelines(GLsizei n, const GLuint *pipelines)
{
  for(GLsizei i = 0; i < n; i++)
  {
    GLResource res = ProgramPipeRes(GetCtx(), pipelines[i]);
    if(GetResourceManager()->HasCurrentResource(res))
    {
      if(GetResourceManager()->HasResourceRecord(res))
        GetResourceManager()->GetResourceRecord(res)->Delete(GetResourceManager());
      GetResourceManager()->UnregisterResource(res);
    }
  }

  GL.glDeleteProgramPipelines(n, pipelines);
}

#pragma endregion

#pragma region ARB_shading_language_include

template <typename SerialiserType>
bool WrappedOpenGL::Serialise_glCompileShaderIncludeARB(SerialiserType &ser, GLuint shaderHandle,
                                                        GLsizei count, const GLchar *const *path,
                                                        const GLint *length)
{
  SERIALISE_ELEMENT_LOCAL(shader, ShaderRes(GetCtx(), shaderHandle));

  SERIALISE_ELEMENT(count);
  SERIALISE_ELEMENT_ARRAY(path, count);
  SERIALISE_ELEMENT_ARRAY(length, count);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    ResourceId liveId = GetResourceManager()->GetID(shader);

    auto &shadDetails = m_Shaders[liveId];

    shadDetails.includepaths.clear();
    shadDetails.includepaths.reserve(count);

    for(int32_t i = 0; i < count; i++)
      shadDetails.includepaths.push_back(path[i]);

    GL.glCompileShaderIncludeARB(shader.name, count, path, NULL);

	// defer ProcessCompilation to Serialise_glLinkProgram
    /*shadDetails.ProcessCompilation(*this, GetResourceManager()->GetOriginalID(liveId), shader.name);*/

    AddResourceInitChunk(shader);
  }

  return true;
}

void WrappedOpenGL::glCompileShaderIncludeARB(GLuint shader, GLsizei count,
                                              const GLchar *const *path, const GLint *length)
{
  SERIALISE_TIME_CALL(GL.glCompileShaderIncludeARB(shader, count, path, length));

  if(IsCaptureMode(m_State))
  {
    GLResourceRecord *record = GetResourceManager()->GetResourceRecord(ShaderRes(GetCtx(), shader));
    RDCASSERTMSG("Couldn't identify object passed to function. Mismatched or bad GLuint?", record,
                 shader);
    if(record)
    {
      USE_SCRATCH_SERIALISER();
      SCOPED_SERIALISE_CHUNK(gl_CurChunk);
      Serialise_glCompileShaderIncludeARB(ser, shader, count, path, length);

      record->AddChunk(scope.Get());
    }
  }
  else
  {
    ResourceId id = GetResourceManager()->GetID(ShaderRes(GetCtx(), shader));

    auto &shadDetails = m_Shaders[id];

    shadDetails.includepaths.clear();
    shadDetails.includepaths.reserve(count);

    for(int32_t i = 0; i < count; i++)
      shadDetails.includepaths.push_back(path[i]);

    shadDetails.ProcessCompilation(*this, id, shader);
  }
}

template <typename SerialiserType>
bool WrappedOpenGL::Serialise_glNamedStringARB(SerialiserType &ser, GLenum type, GLint namelen,
                                               const GLchar *nameStr, GLint stringlen,
                                               const GLchar *valStr)
{
  SERIALISE_ELEMENT(type);
  SERIALISE_ELEMENT(namelen);
  SERIALISE_ELEMENT_LOCAL(
      name, nameStr ? std::string(nameStr, nameStr + (namelen > 0 ? namelen : strlen(nameStr))) : "");
  SERIALISE_ELEMENT(stringlen);
  SERIALISE_ELEMENT_LOCAL(
      value,
      valStr ? std::string(valStr, valStr + (stringlen > 0 ? stringlen : strlen(valStr))) : "");

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    GL.glNamedStringARB(type, (GLint)name.length(), name.c_str(), (GLint)value.length(),
                        value.c_str());
  }

  return true;
}

void WrappedOpenGL::glNamedStringARB(GLenum type, GLint namelen, const GLchar *name,
                                     GLint stringlen, const GLchar *str)
{
  SERIALISE_TIME_CALL(GL.glNamedStringARB(type, namelen, name, stringlen, str));

  if(IsCaptureMode(m_State))
  {
    USE_SCRATCH_SERIALISER();
    SCOPED_SERIALISE_CHUNK(gl_CurChunk);
    Serialise_glNamedStringARB(ser, type, namelen, name, stringlen, str);

    // if a program repeatedly created/destroyed named strings this will fill up with useless
    // strings,
    // but chances are that won't be the case - a few will be created at init time and that's it
    m_DeviceRecord->AddChunk(scope.Get());
  }
}

template <typename SerialiserType>
bool WrappedOpenGL::Serialise_glDeleteNamedStringARB(SerialiserType &ser, GLint namelen,
                                                     const GLchar *nameStr)
{
  SERIALISE_ELEMENT(namelen);
  SERIALISE_ELEMENT_LOCAL(
      name, nameStr ? std::string(nameStr, nameStr + (namelen > 0 ? namelen : strlen(nameStr))) : "");

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    GL.glDeleteNamedStringARB((GLint)name.length(), name.c_str());
  }

  return true;
}

void WrappedOpenGL::glDeleteNamedStringARB(GLint namelen, const GLchar *name)
{
  SERIALISE_TIME_CALL(GL.glDeleteNamedStringARB(namelen, name));

  if(IsCaptureMode(m_State))
  {
    USE_SCRATCH_SERIALISER();
    SCOPED_SERIALISE_CHUNK(gl_CurChunk);
    Serialise_glDeleteNamedStringARB(ser, namelen, name);

    // if a program repeatedly created/destroyed named strings this will fill up with useless
    // strings,
    // but chances are that won't be the case - a few will be created at init time and that's it
    m_DeviceRecord->AddChunk(scope.Get());
  }
}

#pragma endregion

void WrappedOpenGL::glMaxShaderCompilerThreadsKHR(GLuint count)
{
  // pass through, don't record
  GL.glMaxShaderCompilerThreadsKHR(count);
}

template <typename SerialiserType>
bool WrappedOpenGL::Serialise_glSpecializeShader(SerialiserType &ser, GLuint shaderHandle,
                                                 const GLchar *pEntryPoint,
                                                 GLuint numSpecializationConstants,
                                                 const GLuint *pConstantIndex,
                                                 const GLuint *pConstantValue)
{
  SERIALISE_ELEMENT_LOCAL(shader, ShaderRes(GetCtx(), shaderHandle));
  SERIALISE_ELEMENT(pEntryPoint);
  SERIALISE_ELEMENT(numSpecializationConstants);
  SERIALISE_ELEMENT_ARRAY(pConstantIndex, numSpecializationConstants);
  SERIALISE_ELEMENT_ARRAY(pConstantValue, numSpecializationConstants);

  SERIALISE_CHECK_READ_ERRORS();

  if(IsReplayingAndReading())
  {
    ResourceId liveId = GetResourceManager()->GetID(shader);

    GL.glSpecializeShader(shader.name, pEntryPoint, numSpecializationConstants, pConstantIndex,
                          pConstantValue);

    ParseSPIRV(m_Shaders[liveId].spirvWords.data(), m_Shaders[liveId].spirvWords.size(),
               m_Shaders[liveId].spirv);

    m_Shaders[liveId].ProcessSPIRVCompilation(*this, GetResourceManager()->GetOriginalID(liveId),
                                              shader.name, pEntryPoint, numSpecializationConstants,
                                              pConstantIndex, pConstantValue);

    AddResourceInitChunk(shader);
  }

  return true;
}

void WrappedOpenGL::glSpecializeShader(GLuint shader, const GLchar *pEntryPoint,
                                       GLuint numSpecializationConstants,
                                       const GLuint *pConstantIndex, const GLuint *pConstantValue)
{
  SERIALISE_TIME_CALL(GL.glSpecializeShader(shader, pEntryPoint, numSpecializationConstants,
                                            pConstantIndex, pConstantValue));

  if(IsCaptureMode(m_State))
  {
    GLResourceRecord *record = GetResourceManager()->GetResourceRecord(ShaderRes(GetCtx(), shader));
    RDCASSERTMSG("Couldn't identify object passed to function. Mismatched or bad GLuint?", record,
                 shader);
    if(record)
    {
      USE_SCRATCH_SERIALISER();
      SCOPED_SERIALISE_CHUNK(gl_CurChunk);
      Serialise_glSpecializeShader(ser, shader, pEntryPoint, numSpecializationConstants,
                                   pConstantIndex, pConstantValue);

      record->AddChunk(scope.Get());
    }
  }
}

INSTANTIATE_FUNCTION_SERIALISED(void, glCreateShader, GLenum type, GLuint shader);
INSTANTIATE_FUNCTION_SERIALISED(void, glShaderSource, GLuint shaderHandle, GLsizei count,
                                const GLchar *const *source, const GLint *length);
INSTANTIATE_FUNCTION_SERIALISED(void, glCompileShader, GLuint shaderHandle);
INSTANTIATE_FUNCTION_SERIALISED(void, glAttachShader, GLuint programHandle, GLuint shaderHandle);
INSTANTIATE_FUNCTION_SERIALISED(void, glDetachShader, GLuint programHandle, GLuint shaderHandle);
INSTANTIATE_FUNCTION_SERIALISED(void, glCreateShaderProgramv, GLenum type, GLsizei count,
                                const GLchar *const *strings, GLuint program);
INSTANTIATE_FUNCTION_SERIALISED(void, glCreateProgram, GLuint program);
INSTANTIATE_FUNCTION_SERIALISED(void, glLinkProgram, GLuint programHandle);
INSTANTIATE_FUNCTION_SERIALISED(void, glUniformBlockBinding, GLuint programHandle,
                                GLuint uniformBlockIndex, GLuint uniformBlockBinding);
INSTANTIATE_FUNCTION_SERIALISED(void, glShaderStorageBlockBinding, GLuint programHandle,
                                GLuint storageBlockIndex, GLuint storageBlockBinding);
INSTANTIATE_FUNCTION_SERIALISED(void, glBindAttribLocation, GLuint programHandle, GLuint index,
                                const GLchar *name);
INSTANTIATE_FUNCTION_SERIALISED(void, glBindFragDataLocation, GLuint programHandle, GLuint color,
                                const GLchar *name);
INSTANTIATE_FUNCTION_SERIALISED(void, glUniformSubroutinesuiv, GLenum shadertype, GLsizei count,
                                const GLuint *indices);
INSTANTIATE_FUNCTION_SERIALISED(void, glBindFragDataLocationIndexed, GLuint programHandle,
                                GLuint colorNumber, GLuint index, const GLchar *name);
INSTANTIATE_FUNCTION_SERIALISED(void, glTransformFeedbackVaryings, GLuint programHandle,
                                GLsizei count, const GLchar *const *varyings, GLenum bufferMode);
INSTANTIATE_FUNCTION_SERIALISED(void, glProgramParameteri, GLuint programHandle, GLenum pname,
                                GLint value);
INSTANTIATE_FUNCTION_SERIALISED(void, glUseProgram, GLuint programHandle);
INSTANTIATE_FUNCTION_SERIALISED(void, glUseProgramStages, GLuint pipelineHandle, GLbitfield stages,
                                GLuint programHandle);
INSTANTIATE_FUNCTION_SERIALISED(void, glGenProgramPipelines, GLsizei n, GLuint *pipelines);
INSTANTIATE_FUNCTION_SERIALISED(void, glCreateProgramPipelines, GLsizei n, GLuint *pipelines);
INSTANTIATE_FUNCTION_SERIALISED(void, glBindProgramPipeline, GLuint pipelineHandle);
INSTANTIATE_FUNCTION_SERIALISED(void, glCompileShaderIncludeARB, GLuint shaderHandle, GLsizei count,
                                const GLchar *const *path, const GLint *length);
INSTANTIATE_FUNCTION_SERIALISED(void, glNamedStringARB, GLenum type, GLint namelen,
                                const GLchar *nameStr, GLint stringlen, const GLchar *valStr);
INSTANTIATE_FUNCTION_SERIALISED(void, glDeleteNamedStringARB, GLint namelen, const GLchar *nameStr);
INSTANTIATE_FUNCTION_SERIALISED(void, glShaderBinary, GLsizei count, const GLuint *shaders,
                                GLenum binaryformat, const void *binary, GLsizei length);
INSTANTIATE_FUNCTION_SERIALISED(void, glSpecializeShader, GLuint shader, const GLchar *pEntryPoint,
                                GLuint numSpecializationConstants, const GLuint *pConstantIndex,
                                const GLuint *pConstantValue);