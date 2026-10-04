package dev.skycraft.client.mixin;

import java.util.Map;
import java.util.Queue;
import net.minecraft.client.particle.Particle;
import net.minecraft.client.particle.ParticleEngine;
import net.minecraft.client.particle.ParticleRenderType;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.gen.Accessor;

/** Minecraft 1.21.1: the live particles, by how they're drawn. */
@Mixin(ParticleEngine.class)
public interface ParticleEngineAccessor {
	@Accessor("particles")
	Map<ParticleRenderType, Queue<Particle>> skycraft$particles();
}
