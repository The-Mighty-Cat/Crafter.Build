/*
Crafter.Build
Copyright (C) 2024 Catcrafts

This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Lesser General Public
License as published by the Free Software Foundation; either
version 3 of the License, or (at your option) any later version.

This library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public
License along with this library; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301
USA
*/

module;
#include <vector>
#include <string>
#include <print>
#include <fstream>
#include <iostream>
#include "json.hpp"
#include <filesystem>
#include <thread>
module Crafter.Build;
using namespace Crafter::Build;
namespace fs = std::filesystem;

Project::Project(std::string name, fs::path path, std::vector<Configuration> configurations) : name(name), path(path), configurations(configurations) {

}

void Project::Build(std::string configuration) const {
    for(const Configuration& config : configurations) {
        if(config.name == configuration){
            Build(config, config.outputDir);
            return;
        }
    }
    throw std::runtime_error("Configuration: " + configuration + " not found.");
}

void Project::Build(std::string configuration, fs::path outputDir) const {
    for(const Configuration& config : configurations) {
        if(config.name == configuration){
            Build(config, outputDir);
            return;
        }
    }
    throw std::runtime_error("Configuration: " + configuration + " not found.");
}

void Project::Build(Configuration configuration) const {
    Build(configuration, configuration.outputDir);
}

void Project::Build(Configuration config, fs::path outputDir) const {
    if (!fs::exists(config.buildDir)) {
        fs::create_directory(config.buildDir);
    }
    if (!fs::exists(outputDir)) {
        fs::create_directory(outputDir);
    }

    std::string target;
    if(!config.target.empty()){
        target = std::format("-target {}", config.target);
    }
    fs::path pcmDir;
    if(config.type == "library" || config.type == "shared-library"){
        pcmDir = outputDir;
    }else{
        pcmDir = config.buildDir;
    }

    std::string libs;

    std::vector<std::thread> depThreads = std::vector<std::thread>(config.dependencies.size());

    if(config.dependencies.size() > 0){
        libs += std::format(" -L{}", pcmDir.generic_string());
    }

    for(std::int_fast32_t i = 0; i < depThreads.size(); i++) {
        Project project = Project::LoadFromJSON(config.dependencies[i].path);
        libs+=std::format(" -l{}", project.name);
        depThreads[i] = std::thread([i, pcmDir, config, project]() {
            project.Build(config.dependencies[i].configuration, pcmDir);
        });
    }

    std::string name = this->name;

    std::string clangDir;
    if(config.target == "wasm32-unknown-wasi" || config.target == "wasm64-unknown-wasi"){
        clangDir = "${WASI_SDK_PATH}/bin/clang++ --sysroot=${WASI_SDK_PATH}/share/wasi-sysroot -Wno-unused-command-line-argument";
        name+=".wasm";
    } else if(config.target == "wasm32" || config.target == "wasm64") {
        clangDir = "${WASI_SDK_PATH}/bin/clang++ --no-standard-libraries -Wl,--no-entry -Wl,--export-all -Wno-unused-command-line-argument";
        name+=".wasm";
    } else {
        clangDir = "clang++ -Wno-unused-command-line-argument";
    }

    for(std::thread& thread : depThreads){
        thread.join();
    }

    //clangDir+= std::format(" -I {} ", pcmDir);

    std::vector<fs::path> updatedModules;
    std::string files;
    for(const fs::path& moduleFile : config.moduleFiles){
        if(!fs::exists((pcmDir/moduleFile.filename()).generic_string()+".pcm") || fs::last_write_time(moduleFile.generic_string()+".cppm") > fs::last_write_time((pcmDir/moduleFile.filename()).generic_string()+".pcm")) {
            updatedModules.push_back(pcmDir/moduleFile.filename());
            system(std::format("{} -std={} {}.cppm --precompile -fprebuilt-module-path={} -o {}.pcm {}", clangDir, config.standard, moduleFile.generic_string(), pcmDir.generic_string(), (pcmDir/moduleFile.filename()).generic_string(), target).c_str());
        }
        files+=std::format("{}.o ",(config.buildDir/moduleFile.filename()).generic_string());
    }
    std::vector<std::thread> threads;
    for(const fs::path moduleFile : updatedModules) {
        threads.emplace_back([moduleFile, config, pcmDir, target, clangDir](){
            system(std::format("{} -std={} {}.pcm -fprebuilt-module-path={} -c -O{} -o {}.o {}", clangDir, config.standard, (pcmDir/moduleFile.filename()).generic_string(), pcmDir.generic_string(), config.optimizationLevel, (config.buildDir/moduleFile.filename()).generic_string(), target).c_str());
        });
    }
    for(const fs::path& soureFile : config.sourceFiles) {
        files+=std::format("{}_source.o ",(config.buildDir/soureFile.filename()).generic_string());
        if(!fs::exists((config.buildDir/soureFile.filename()).generic_string()+"_source.o") || fs::last_write_time(soureFile.generic_string()+".cpp") > fs::last_write_time((config.buildDir/soureFile.filename()).generic_string()+"_source.o")) {
            threads.emplace_back([soureFile, config, pcmDir, target, clangDir](){
                system(std::format("{} -std={} {}.cpp -fprebuilt-module-path={} -c -O{} -o {}_source.o {}", clangDir, config.standard, soureFile.generic_string(), pcmDir.generic_string(), config.optimizationLevel, (config.buildDir/soureFile.filename()).generic_string(), target).c_str());
            });
        }
    }
    for(std::thread& thread : threads){
        thread.join();
    }
    if(config.type == "executable"){
         system(std::format("{} {}-O{} -o {} {} {}", clangDir, files, config.optimizationLevel, (outputDir/name).generic_string(), target, libs).c_str());
    } else if(config.type == "library"){
         system(std::format("ar r {}.a {}", (outputDir/fs::path("lib"+name)).generic_string(), files).c_str());
    } else if(config.type == "shared-library"){
        system(std::format("ar r {}.so {} -shared", (outputDir/fs::path("lib"+name)).generic_string(), files).c_str());
    }

}

Project Project::LoadFromJSON(fs::path path) {
     if (!fs::exists(path)) {
         throw std::runtime_error(std::format("Project file: {} not found.", path.generic_string()));
     }

    std::ifstream f(path);
    nlohmann::json data = nlohmann::json::parse(f);
    const std::string name = data["name"].get<std::string>();
    std::vector<Configuration> configurations;
    nlohmann::json configs = data["configurations"];

    const fs::path workingDir = path.remove_filename();
    for (nlohmann::json::iterator it = configs.begin(); it != configs.end(); ++it) {
        configurations.emplace_back(configs, (*it), workingDir);
    }
    return Project(name, workingDir, configurations);
}
