# Copyright 2014-2023 the openage authors. See copying.md for legal info.

""" Routines for texture generation etc """

# TODO pylint: disable=C,R

from __future__ import annotations
import typing

from PIL import Image

import numpy

from ....log import spam
from ...value_object.read.media.blendomatic import BlendingMode
from ...value_object.read.media.hardcoded.terrain_tile_size import TILE_HALFSIZE
from ...value_object.read.genie_structure import GenieStructure

if typing.TYPE_CHECKING:
    from openage.convert.value_object.read.media.colortable import ColorTable
    from openage.convert.service.export.interface.cutter import InterfaceCutter
    from openage.convert.value_object.read.media.slp import SLP, SLPFrame
    from openage.convert.value_object.read.media.smp import SMP, SMPLayer
    from openage.convert.value_object.read.media.smx import SMX, SMXLayer
    from openage.convert.value_object.read.media.sld import SLD, SLDLayer


class TextureImage:
    """
    represents a image created from a (r,g,b,a) matrix.
    """

    def __init__(
        self,
        picture_data: typing.Union[Image.Image, numpy.ndarray],
        hotspot: tuple[int, int] = None
    ):

        if isinstance(picture_data, Image.Image):
            if picture_data.mode != 'RGBA':
                picture_data = picture_data.convert('RGBA')

            picture_data = numpy.array(picture_data)

        if not isinstance(picture_data, numpy.ndarray):
            raise ValueError(
                f"Texture image must be created from PIL Image or numpy array, not '{type(picture_data)}'"
            )

        self.height: int = picture_data.shape[0]

        self.width: int = picture_data.shape[1]
        spam("creating TextureImage with size %d x %d", self.width, self.height)

        self.hotspot = (0, 0) if hotspot is None else hotspot
        self.data = picture_data

    def get_pil_image(self) -> Image.Image:
        return Image.fromarray(self.data)

    def get_data(self) -> numpy.ndarray:
        return self.data


class Texture(GenieStructure):
    image_format = "png"

    name_struct = "subtexture"
    name_struct_file = "texture"
    struct_description = (
        "one sprite, as part of a texture atlas.\n"
        "\n"
        "this struct stores information about positions and sizes\n"
        "of sprites included in the 'big texture'."
    )

    def __init__(
        self,
        input_data: typing.Union[SLP, SMP, SMX, SLD, BlendingMode],
        palettes: dict[int, ColorTable] = None,
        custom_cutter: InterfaceCutter = None,
        layer: int = 0
    ):
        super().__init__()

        # Best packer hints (positions of sprites in texture)
        self.best_packer_hints: tuple = None

        self.image_data: TextureImage = None
        self.image_metadata: list[dict[str, int]] = {}

        self.best_compr: tuple = None
        spam("creating Texture from %s", repr(input_data))

        from ...value_object.read.media.slp import SLP
        from ...value_object.read.media.smp import SMP
        from ...value_object.read.media.smx import SMX
        from ...value_object.read.media.sld import SLD

        self.frames = []
        if isinstance(input_data, (SLP, SMP, SMX)):
            input_frames = input_data.get_frames(layer)
            for frame in input_frames:
                # Palette can be different for every frame
                palette_number = frame.get_palette_number()

                if palette_number is None:
                    main_palette = None

                else:
                    main_palette = palettes[palette_number].array

                self.frames.extend(
                    iter(self._to_subtextures(frame, main_palette, custom_cutter))
                )
        elif isinstance(input_data, SLD):
            input_frames = input_data.get_frames(layer)
            if layer == 0 and len(input_frames) == 0:
                # Use shadows if no main graphics are inside
                input_frames = input_data.get_frames(layer=1)

            for frame in input_frames:
                subtex = TextureImage(
                    frame.get_picture_data(),
                    hotspot=frame.get_hotspot()
                )
                self.frames.append(subtex)

        elif isinstance(input_data, BlendingMode):
            self.frames = [
                # the hotspot is in the west corner of a tile.
                TextureImage(
                    tile.get_picture_data(),
                    hotspot=(0, TILE_HALFSIZE["y"])
                )
                for tile in input_data.alphamasks
            ]
        else:
            raise Exception(
                f"cannot create Texture from unknown source type: {type(input_data)}"
            )

    def _to_subtextures(
        self,
        frame: typing.Union[SLPFrame, SMPLayer, SMXLayer],
        main_palette: ColorTable,
        custom_cutter: InterfaceCutter = None
    ):
        """
        convert slp to subtexture or subtextures, using a palette.
        """
        subtex = TextureImage(
            frame.get_picture_data(main_palette),
            hotspot=frame.get_hotspot()
        )

        return custom_cutter.cut(subtex) if custom_cutter else [subtex]

    def get_metadata(self) -> list[dict[str, int]]:
        """
        Get the image metadata information.
        """
        return self.image_metadata

    def get_cache_params(self) -> tuple[tuple, tuple]:
        """
        Get the parameters used for packing and saving the texture.
            - Packing hints (sprite index, (xpos, ypos) in the final texture)
            - PNG compression parameters (compression level + deflate params)
        """
        return self.best_packer_hints, self.best_compr

    @classmethod
    def get_data_format_members(cls, game_version) -> tuple:
        """
        Return the members in this struct.
        """
        return (
            (True, "x", None, "int32_t"),
            (True, "y", None, "int32_t"),
            (True, "w", None, "int32_t"),
            (True, "h", None, "int32_t"),
            (True, "cx", None, "int32_t"),
            (True, "cy", None, "int32_t"),
        )
