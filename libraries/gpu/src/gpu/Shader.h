//
//  Shader.h
//  libraries/gpu/src/gpu
//
//  Created by Sam Gateau on 2/27/2015.
//  Copyright 2014 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//
#ifndef hifi_gpu_Shader_h
#define hifi_gpu_Shader_h

#include "Resource.h"
#include <memory>
#include <set>
 
namespace gpu {

class Shader {
public:

    typedef QSharedPointer< Shader > Pointer;
    typedef std::vector< Pointer > Shaders;

    class Source {
    public:
        enum Language {
            GLSL = 0,
        };

        Source() {}
        Source(const std::string& code, Language lang = GLSL) : _code(code), _lang(lang) {}
        Source(const Source& source) : _code(source._code), _lang(source._lang) {}
        virtual ~Source() {}
 
        virtual const std::string& getCode() const { return _code; }

    protected:
        std::string _code;
        Language _lang = GLSL;
    };

    class Slot {
    public:

        std::string _name;
        uint16 _location;
        Element _element;
 
        Slot(const std::string& name, uint16 location, const Element& element) : _name(name), _location(location), _element(element) {}


        class Less {
        public:
            bool operator() (const Slot& x, const Slot& y) const { return x._name < y._name; }
        };
    };
    typedef std::set<Slot, Slot::Less> SlotSet;


    enum Type {
        VERTEX = 0,
        PIXEL,
        GEOMETRY,
        NUM_DOMAINS,

        PROGRAM,
    };

    static Shader* createVertex(const Source& source);
    static Shader* createPixel(const Source& source);

    static Shader* createProgram(Pointer& vertexShader, Pointer& pixelShader);


    ~Shader();

    Type getType() const { return _type; }
    bool isProgram() const { return getType() > NUM_DOMAINS; }
    bool isDomain() const { return getType() < NUM_DOMAINS; }

    const Source& getSource() const { return _source; }

    const Shaders& getShaders() const { return _shaders; }

    // Access the exposed uniform, input and output slot
    const SlotSet& getUniforms() const { return _uniforms; }
    const SlotSet& getBuffers() const { return _buffers; }
    const SlotSet& getTextures() const { return _textures; }
    const SlotSet& getSamplers() const { return _samplers; }
    const SlotSet& getInputs() const { return _inputs; }
    const SlotSet& getOutputs() const { return _outputs; }

    // Define the list of uniforms, inputs and outputs for the shader
    // This call is intendend to build the list of exposed slots in order
    // to correctly bind resource to the shader.
    // These can be build "manually" from knowledge of the atual shader code
    // or automatically by calling "Context::makeShader()", this is the preferred way
    void defineSlots(const SlotSet& uniforms, const SlotSet& buffers, const SlotSet& textures, const SlotSet& samplers, const SlotSet& inputs, const SlotSet& outputs);


protected:
    Shader(Type type, const Source& source);
    Shader(Type type, Pointer& vertex, Pointer& pixel);

    Shader(const Shader& shader); // deep copy of the sysmem shader
    Shader& operator=(const Shader& shader); // deep copy of the sysmem texture

    // Source contains the actual source code or nothing if the shader is a program
    Source _source;

    // if shader is composed of sub shaders, here they are
    Shaders _shaders;

    // List of exposed uniform, input and output slots
    SlotSet _uniforms;
    SlotSet _buffers;
    SlotSet _textures;
    SlotSet _samplers;
    SlotSet _inputs;
    SlotSet _outputs;

    // The type of the shader, the master key
    Type _type;

    // This shouldn't be used by anything else than the Backend class with the proper casting.
    mutable GPUObject* _gpuObject = NULL;
    void setGPUObject(GPUObject* gpuObject) const { _gpuObject = gpuObject; }
    GPUObject* getGPUObject() const { return _gpuObject; }
    friend class Backend;
};

typedef Shader::Pointer ShaderPointer;
typedef std::vector< ShaderPointer > Shaders;

};


#endif
