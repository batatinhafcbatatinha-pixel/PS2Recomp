include(CheckIPOSupported)

check_ipo_supported(RESULT IPO_SUPPORTED OUTPUT IPO_ERROR)

# LTCG over the full recompiled game needs >10 GB RAM and hours single-threaded.
option(PS2X_ENABLE_LTCG "Whole-program optimization (/GL + /LTCG) in Release" ON)
# Identical-COMDAT folding over ~80k generated functions makes every relink slow
# and saves little; skip it unless a release build asks for it.
option(PS2X_FAST_LINK "Skip /OPT:ICF so relinks are faster" OFF)

function(EnableFastReleaseMode TargetName)
    message("> Enabling optimization for: ${TargetName}")
    if(MSVC)
        target_compile_options(${TargetName} PRIVATE
            $<$<CONFIG:Release>:
                /O2 # speed
                /Ob2 # inline aggressively
                /Oi # intrinsics
                /Gy # function-level linking
                /Gw # global data in COMDAT
                /GF # string pooling
                /Zc:inline # remove unreferenced inline
                /fp:fast # fast math (graphics friendly)
                /DNDEBUG
                /arch:AVX2 # Advanced Vector Extensions 2
                /GS- # Disable Buffer Security Check (faster)
                /Qspectre- # Disable Spectre mitigations (faster)
            >
        )

        if(PS2X_ENABLE_LTCG)
            target_compile_options(${TargetName} PRIVATE $<$<CONFIG:Release>:/GL>)
            target_link_options(${TargetName} PRIVATE $<$<CONFIG:Release>:/LTCG>)
        endif()

        if(TARGET ${TargetName})
            target_link_options(${TargetName} PRIVATE
                $<$<CONFIG:Release>:
                    /OPT:REF # remove unreferenced
                >
            )
            if(PS2X_FAST_LINK)
                target_link_options(${TargetName} PRIVATE $<$<CONFIG:Release>:/OPT:NOICF>)
            else()
                target_link_options(${TargetName} PRIVATE $<$<CONFIG:Release>:/OPT:ICF>)
            endif()
        endif()
    endif()

    if(NOT PS2X_ENABLE_LTCG)
        set_property(TARGET ${TargetName} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE FALSE)
    elseif(IPO_SUPPORTED)
        set_property(TARGET ${TargetName} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    else()
        message(WARNING "Interprocedural optimization not supported: ${ipo_error}")
    endif()
endfunction()